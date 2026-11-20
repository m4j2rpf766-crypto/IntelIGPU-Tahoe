#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#import <IOKit/IOKitLib.h>
#include <stdio.h>
#include <string.h>

extern OSStatus WEXRegisterIntelHEVCEncoder(void);
static NSString * const expectedEncoderID = @"org.geekaudio.whateverxe.videoencoder.hevc";

static int discover(uint64_t *listedGPU, bool *listedHardware, bool *found) {
    CFArrayRef list = NULL;
    OSStatus status = VTCopyVideoEncoderList(NULL, &list);
    *found = false;
    *listedGPU = 0;
    *listedHardware = false;
    if (status || !list) return 4;
    for (NSDictionary *item in (__bridge NSArray *)list) {
        if (![item[(__bridge NSString *)kVTVideoEncoderList_EncoderID] isEqual:expectedEncoderID]) continue;
        *found = true;
        *listedGPU = [item[(__bridge NSString *)kVTVideoEncoderList_GPURegistryID] unsignedLongLongValue];
        *listedHardware = [item[(__bridge NSString *)kVTVideoEncoderList_IsHardwareAccelerated] boolValue];
        break;
    }
    CFRelease(list);
    printf("listed=%u gpu=0x%llx hardware=%u\n", *found, (unsigned long long)*listedGPU, *listedHardware);
    return 0;
}

typedef struct {
    FILE *output;
    int frames;
    int errors;
} Result;

static bool writeNAL(FILE *out, const uint8_t *bytes, size_t count) {
    const uint8_t prefix[] = {0, 0, 0, 1};
    return count > 0 && fwrite(prefix, 1, sizeof(prefix), out) == sizeof(prefix)
        && fwrite(bytes, 1, count, out) == count;
}

static bool writeSample(FILE *out, CMSampleBufferRef sample, bool firstFrame) {
    CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sample);
    size_t setCount = 0;
    int lengthBytes = 0;
    if (!fmt || CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fmt, 0, NULL,
            NULL, &setCount, &lengthBytes) || setCount < 3 || lengthBytes < 1 ||
            lengthBytes > 4) return false;
    if (firstFrame) {
        for (size_t i = 0; i < setCount; i++) {
            const uint8_t *bytes = NULL;
            size_t count = 0;
            if (CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fmt, i,
                    &bytes, &count, NULL, NULL) || !writeNAL(out, bytes, count)) return false;
        }
    }
    CMBlockBufferRef data = CMSampleBufferGetDataBuffer(sample);
    size_t total = data ? CMBlockBufferGetDataLength(data) : 0;
    if (!total || total > 16 * 1024 * 1024) return false;
    NSMutableData *copy = [NSMutableData dataWithLength:total];
    if (CMBlockBufferCopyDataBytes(data, 0, total, copy.mutableBytes)) return false;
    const uint8_t *bytes = copy.bytes;
    for (size_t pos = 0; pos < total;) {
        uint32_t n = 0;
        if ((size_t)lengthBytes > total - pos) return false;
        for (int i = 0; i < lengthBytes; i++) n = (n << 8) | bytes[pos++];
        if (n > total - pos || !writeNAL(out, bytes + pos, n)) return false;
        pos += n;
    }
    return true;
}

static void outputFrame(void *opaque, void *frameRef, OSStatus status,
                        VTEncodeInfoFlags flags, CMSampleBufferRef sample) {
    (void)frameRef;
    Result *result = opaque;
    if (status || (flags & kVTEncodeInfo_FrameDropped) || !sample ||
        !CMSampleBufferDataIsReady(sample) ||
        CMTimeCompare(CMSampleBufferGetPresentationTimeStamp(sample),
                      CMTimeMake(result->frames * 1001LL, 30000)) != 0 ||
        !writeSample(result->output, sample, result->frames == 0)) {
        ++result->errors;
        return;
    }
    ++result->frames;
}

static OSStatus readRows(FILE *input, uint8_t *row, size_t stride, size_t width, size_t rows) {
    if (!row || stride < width) return kVTParameterErr;
    for (size_t y = 0; y < rows; y++) {
        if (fread(row, 1, width, input) != width) return kVTParameterErr;
        row += stride;
    }
    return noErr;
}

static int check(FILE *input, FILE *output, int count, bool requireGPU, OSType format) {
    const int width = 1920, height = 1080;
    uint64_t rid = 0;
    io_service_t accel = IOServiceGetMatchingService(kIOMainPortDefault,
                                                     IOServiceMatching("IntelAccelerator"));
    if (!accel) return 2;
    kern_return_t kr = IORegistryEntryGetRegistryEntryID(accel, &rid);
    IOObjectRelease(accel);
    if (kr != KERN_SUCCESS) return 2;
    OSStatus status = WEXRegisterIntelHEVCEncoder();
    printf("register=%d\n", (int)status);
    if (status) return 3;
    bool listed = false, listedHardware = false;
    uint64_t listedGPU = 0;
    if (discover(&listedGPU, &listedHardware, &listed) || !listed || !listedHardware ||
        (listedGPU && listedGPU != rid)) return 4;
    NSMutableDictionary *spec = [@{(__bridge NSString *)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder:@YES} mutableCopy];
    if (requireGPU) spec[(__bridge NSString *)kVTVideoEncoderSpecification_RequiredEncoderGPURegistryID] = @(rid);
    NSDictionary *attrs = @{(__bridge NSString *)kCVPixelBufferPixelFormatTypeKey:@(format),
                            (__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey:@{}};
    Result result = {output, 0, 0};
    VTCompressionSessionRef vt = NULL;
    status = VTCompressionSessionCreate(NULL, width, height, kCMVideoCodecType_HEVC,
        (__bridge CFDictionaryRef)spec, (__bridge CFDictionaryRef)attrs, NULL,
        outputFrame, &result, &vt);
    printf("session=%d\n", (int)status);
    if (status || !vt) return 5;
    CFTypeRef selected = NULL, hw = NULL;
    VTSessionCopyProperty(vt, kVTCompressionPropertyKey_EncoderID, NULL, &selected);
    VTSessionCopyProperty(vt, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder, NULL, &hw);
    bool correct = [expectedEncoderID isEqual:(__bridge id)selected] && hw == kCFBooleanTrue;
    printf("selected=%s hardware=%d\n", [[(__bridge id)selected description] UTF8String] ?: "unknown", hw == kCFBooleanTrue);
    if (hw) CFRelease(hw);
    if (selected) CFRelease(selected);
    NSDictionary *props = @{(__bridge NSString *)kVTCompressionPropertyKey_AverageBitRate:@5500000,
                            (__bridge NSString *)kVTCompressionPropertyKey_ExpectedFrameRate:@(30000.0/1001.0),
                            (__bridge NSString *)kVTCompressionPropertyKey_AllowFrameReordering:@NO,
                            (__bridge NSString *)kVTCompressionPropertyKey_ProfileLevel:(__bridge NSString *)kVTProfileLevel_HEVC_Main_AutoLevel};
    status = correct ? VTSessionSetProperties(vt, (__bridge CFDictionaryRef)props) : kVTCouldNotFindVideoEncoderErr;
    if (!status) status = VTCompressionSessionPrepareToEncodeFrames(vt);
    for (int i = 0; i < count && !status; i++) {
        @autoreleasepool {
            CVPixelBufferRef image = NULL;
            status = CVPixelBufferCreate(NULL, width, height, format,
                                         (__bridge CFDictionaryRef)attrs, &image);
            if (status) break;
            status = CVPixelBufferLockBaseAddress(image, 0);
            if (!status) {
                if (format == kCVPixelFormatType_32BGRA) {
                    status = readRows(input, CVPixelBufferGetBaseAddress(image),
                        CVPixelBufferGetBytesPerRow(image), 4 * (size_t)width, height);
                } else {
                    for (size_t p = 0; p < 2 && !status; p++) {
                        status = readRows(input, CVPixelBufferGetBaseAddressOfPlane(image, p),
                            CVPixelBufferGetBytesPerRowOfPlane(image, p), width,
                            CVPixelBufferGetHeightOfPlane(image, p));
                    }
                }
                CVPixelBufferUnlockBaseAddress(image, 0);
            }
            if (!status) status = VTCompressionSessionEncodeFrame(vt, image,
                CMTimeMake(i*1001LL,30000), CMTimeMake(1001,30000), NULL, NULL, NULL);
            CVPixelBufferRelease(image);
        }
    }
    if (!status) status = VTCompressionSessionCompleteFrames(vt, kCMTimeInvalid);
    printf("status=%d frames=%d callback_errors=%d\n",(int)status,result.frames,result.errors);
    VTCompressionSessionInvalidate(vt);
    CFRelease(vt);
    return status || result.errors || result.frames != count ? 6 : 0;
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc == 2 && strcmp(argv[1], "expect-unavailable") == 0) {
            OSStatus registration = WEXRegisterIntelHEVCEncoder();
            printf("register=%d\n", (int)registration);
            bool listed = false, hw = false;
            uint64_t gpu = 0;
            if (discover(&gpu, &hw, &listed)) return 4;
            return ((registration == kVTVideoEncoderNotAvailableNowErr ||
                     registration == kVTCouldNotFindVideoEncoderErr) && !listed) ? 0 : 5;
        }
        if ((argc != 5 && argc != 6) || (strcmp(argv[1], "auto") && strcmp(argv[1], "intel")) ||
            (argc == 6 && strcmp(argv[5], "bgra") && strcmp(argv[5], "nv12"))) {
            fputs("usage: check_provider expect-unavailable\n"
                  "       check_provider auto|intel input.raw output.h265 frames [bgra|nv12]\n",stderr);
            return 2;
        }
        char *end = NULL;
        long count = strtol(argv[4], &end, 10);
        if (!end || *end || count < 1 || count > 600) return 2;
        FILE *in = fopen(argv[2],"rb");
        if (!in) return 2;
        FILE *out = fopen(argv[3],"wb");
        OSType format = argc == 6 && !strcmp(argv[5],"bgra") ? kCVPixelFormatType_32BGRA :
            kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        int result = out ? check(in,out,(int)count,!strcmp(argv[1],"intel"),format) : 2;
        fclose(in);
        if (out && fclose(out)) result = 2;
        return result;
    }
}
