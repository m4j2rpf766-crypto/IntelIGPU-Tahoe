#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define main WEXWorkerExecutableMain
#include "worker.m"
#undef main

static int write_all(FILE *file, const void *data, size_t size) {
    return fwrite(data, 1, size, file) == size;
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
        FILE *records = fopen(argv[2], "wb");
        if (!records) return 6;
        WEXWorker worker = {0};
        uint64_t frames = 0;
        CMSampleBufferRef source = NULL;
        OSStatus status = noErr;
        while ((source = [output copyNextSampleBuffer])) {
            if (CMSampleBufferGetNumSamples(source) == 0) {
                CFRelease(source);
                continue;
            }
            WEXEncodedInfo info = {0};
            uint8_t *payload = NULL;
            uint32_t payloadBytes = 0;
            status = pack_sample(&worker, source, &info, &payload, &payloadBytes);
            if (status != noErr || !payload) {
                fprintf(stderr, "pack frame=%llu status=%d\n",
                    (unsigned long long)frames, (int)status);
                free(payload);
                CFRelease(source);
                fclose(records);
                destroy_worker(&worker);
                return 7;
            }
            uint32_t recordBytes = payloadBytes;
            if (!write_all(records, &recordBytes, sizeof(recordBytes)) ||
                !write_all(records, payload, payloadBytes)) {
                free(payload);
                CFRelease(source);
                fclose(records);
                destroy_worker(&worker);
                return 8;
            }
            free(payload);
            CFRelease(source);
            frames++;
        }
        BOOL completed = reader.status == AVAssetReaderStatusCompleted;
        if (fclose(records) != 0) completed = NO;
        destroy_worker(&worker);
        fprintf(stderr, "packed=%llu reader_status=%ld pack_status=%d\n",
            (unsigned long long)frames, (long)reader.status, (int)status);
        return completed && frames > 0 ? 0 : 9;
    }
}
