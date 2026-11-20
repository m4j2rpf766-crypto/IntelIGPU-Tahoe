#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>
#include <CoreMedia/CMBlockBuffer.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "transport.m"

static int read_file_exact(FILE *file, void *bytes, size_t length) {
    return fread(bytes, 1, length, file) == length;
}

static int coded_bytes_equal(CMSampleBufferRef left, CMSampleBufferRef right) {
    CMBlockBufferRef a = CMSampleBufferGetDataBuffer(left);
    CMBlockBufferRef b = CMSampleBufferGetDataBuffer(right);
    size_t bytes = a ? CMBlockBufferGetDataLength(a) : 0;
    if (!bytes || !b || CMBlockBufferGetDataLength(b) != bytes) return 0;
    uint8_t *x = malloc(bytes), *y = malloc(bytes);
    if (!x || !y) { free(x); free(y); return 0; }
    OSStatus s1 = CMBlockBufferCopyDataBytes(a, 0, bytes, x);
    OSStatus s2 = CMBlockBufferCopyDataBytes(b, 0, bytes, y);
    int equal = s1 == noErr && s2 == noErr && memcmp(x, y, bytes) == 0;
    free(x); free(y);
    return equal;
}

static int parameter_sets_equal(CMSampleBufferRef left, CMSampleBufferRef right) {
    const uint8_t *a = NULL, *b = NULL;
    size_t sizeA = 0, sizeB = 0, countA = 0, countB = 0;
    int lengthA = 0, lengthB = 0;
    CMFormatDescriptionRef fa = CMSampleBufferGetFormatDescription(left);
    CMFormatDescriptionRef fb = CMSampleBufferGetFormatDescription(right);
    if (!fa || !fb || CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fa, 0,
        &a, &sizeA, &countA, &lengthA) || CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fb, 0,
        &b, &sizeB, &countB, &lengthB) || countA != countB || lengthA != lengthB) return 0;
    for (size_t index = 0; index < countA; index++) {
        if (CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fa, index,
                &a, &sizeA, NULL, NULL) ||
            CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fb, index,
                &b, &sizeB, NULL, NULL) || sizeA != sizeB || memcmp(a, b, sizeA) != 0)
            return 0;
    }
    return 1;
}

static int keyframe(CMSampleBufferRef sample) {
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (!attachments || CFArrayGetCount(attachments) == 0) return 1;
    CFDictionaryRef dict = CFArrayGetValueAtIndex(attachments, 0);
    CFTypeRef notSync = dict ? CFDictionaryGetValue(dict, kCMSampleAttachmentKey_NotSync) : NULL;
    return !notSync || notSync != kCFBooleanTrue;
}

static int format_colors_equal(CMSampleBufferRef left, CMSampleBufferRef right) {
    CFDictionaryRef a = CMFormatDescriptionGetExtensions(CMSampleBufferGetFormatDescription(left));
    CFDictionaryRef b = CMFormatDescriptionGetExtensions(CMSampleBufferGetFormatDescription(right));
    CFStringRef keys[] = { kCMFormatDescriptionExtension_ColorPrimaries,
        kCMFormatDescriptionExtension_TransferFunction,
        kCMFormatDescriptionExtension_YCbCrMatrix };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        CFTypeRef x = a ? CFDictionaryGetValue(a, keys[i]) : NULL;
        CFTypeRef y = b ? CFDictionaryGetValue(b, keys[i]) : NULL;
        if ((x == NULL) != (y == NULL) || (x && !CFEqual(x, y))) return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    @autoreleasepool {
        NSURL *url = [NSURL fileURLWithPath:@(argv[1])];
        AVURLAsset *asset = [AVURLAsset URLAssetWithURL:url options:nil];
        AVAssetTrack *track = [asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
        if (!track) return 3;
        NSError *error = nil;
        AVAssetReader *reader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
        AVAssetReaderTrackOutput *output = [[AVAssetReaderTrackOutput alloc]
            initWithTrack:track outputSettings:nil];
        if (!reader || error || ![reader canAddOutput:output]) return 4;
        [reader addOutput:output];
        if (![reader startReading]) return 5;
        FILE *records = fopen(argv[2], "rb");
        if (!records) return 6;
        uint64_t frames = 0, ptsEqual = 1, durationEqual = 1, keyEqual = 1;
        uint64_t codedEqual = 1, setsEqual = 1, dimensionsEqual = 1, colorsEqual = 1;
        uint64_t bytes = 0, firstKey = 0, inter = 0;
        CMSampleBufferRef source = NULL;
        while ((source = [output copyNextSampleBuffer])) {
            if (CMSampleBufferGetNumSamples(source) == 0) { CFRelease(source); continue; }
            uint32_t recordBytes = 0;
            WEXEncodedInfo info = {0};
            if (!read_file_exact(records, &recordBytes, sizeof(recordBytes)) ||
                recordBytes < sizeof(info) || recordBytes > WEXMaxPacket) {
                CFRelease(source); fclose(records); return 7;
            }
            uint8_t *payload = malloc(recordBytes);
            if (!payload || !read_file_exact(records, payload, recordBytes)) {
                free(payload); CFRelease(source); fclose(records); return 9;
            }
            memcpy(&info, payload, sizeof(info));
            size_t expected = sizeof(info) + (size_t)info.vpsBytes + info.spsBytes +
                info.ppsBytes + info.codedBytes;
            if (expected != recordBytes) {
                free(payload); CFRelease(source); fclose(records); return 8;
            }
            CMSampleBufferRef rebuilt = NULL;
            OSStatus status = make_sample_buffer(&info, payload + sizeof(info), &rebuilt);
            free(payload);
            if (status || !rebuilt) {
                fprintf(stderr, "unpack frame=%llu status=%d\n", (unsigned long long)frames, (int)status);
                if (rebuilt) CFRelease(rebuilt);
                CFRelease(source); fclose(records); return 10;
            }
            ptsEqual &= CMTimeCompare(CMSampleBufferGetPresentationTimeStamp(source),
                                      CMSampleBufferGetPresentationTimeStamp(rebuilt)) == 0;
            durationEqual &= CMTimeCompare(CMSampleBufferGetDuration(source),
                                           CMSampleBufferGetDuration(rebuilt)) == 0;
            int sourceKey = keyframe(source), rebuiltKey = keyframe(rebuilt);
            keyEqual &= sourceKey == rebuiltKey;
            firstKey += sourceKey; inter += !sourceKey;
            codedEqual &= coded_bytes_equal(source, rebuilt);
            setsEqual &= parameter_sets_equal(source, rebuilt);
            CMVideoDimensions a = CMVideoFormatDescriptionGetDimensions(
                CMSampleBufferGetFormatDescription(source));
            CMVideoDimensions b = CMVideoFormatDescriptionGetDimensions(
                CMSampleBufferGetFormatDescription(rebuilt));
            dimensionsEqual &= a.width == b.width && a.height == b.height;
            colorsEqual &= format_colors_equal(source, rebuilt);
            CMBlockBufferRef block = CMSampleBufferGetDataBuffer(source);
            bytes += block ? CMBlockBufferGetDataLength(block) : 0;
            frames++;
            CFRelease(rebuilt);
            CFRelease(source);
        }
        int extra = fgetc(records);
        fclose(records);
        BOOL completed = reader.status == AVAssetReaderStatusCompleted;
        printf("frames=%llu bytes=%llu IDR=%llu inter=%llu pts=%u duration=%u key=%u coded=%u sets=%u dims=%u colors=%u reader=%ld trailing=%d\n",
            (unsigned long long)frames,(unsigned long long)bytes,
            (unsigned long long)firstKey,(unsigned long long)inter,
            (unsigned)ptsEqual,(unsigned)durationEqual,(unsigned)keyEqual,
            (unsigned)codedEqual,(unsigned)setsEqual,(unsigned)dimensionsEqual,
            (unsigned)colorsEqual,(long)reader.status,extra);
        return completed && frames > 0 && extra == EOF && ptsEqual && durationEqual &&
            keyEqual && codedEqual && setsEqual && dimensionsEqual && colorsEqual ? 0 : 11;
    }
}
