#include "pixel_input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { kTestWidth = 8, kTestHeight = 4, kTestRowBytes = 40 };

typedef struct {
    uint8_t blue;
    uint8_t green;
    uint8_t red;
} TestColor;

static const TestColor kBlockColors[] = {
    {0, 0, 0},
    {255, 255, 255},
    {0, 0, 255},
    {0, 255, 0},
    {255, 0, 0},
    {128, 128, 128},
    {20, 80, 220},
    {230, 170, 40}
};

typedef struct {
    uint8_t y[8];
    uint8_t cb[8];
    uint8_t cr[8];
} ExpectedMatrix;

static const ExpectedMatrix kRec709 = {
    .y = {16, 235, 63, 173, 32, 126, 107, 142},
    .cb = {128, 128, 102, 42, 240, 128, 88, 167},
    .cr = {128, 128, 240, 26, 118, 128, 192, 68}
};

static const ExpectedMatrix kRec601 = {
    .y = {16, 235, 81, 145, 41, 126, 115, 135},
    .cb = {128, 128, 90, 54, 240, 128, 81, 174},
    .cr = {128, 128, 240, 34, 110, 128, 194, 67}
};

static const ExpectedMatrix kRec2020 = {
    .y = {16, 235, 74, 165, 29, 126, 113, 136},
    .cb = {128, 128, 97, 47, 240, 128, 84, 170},
    .cr = {128, 128, 240, 25, 119, 128, 192, 69}
};

static void release_bytes(void *releaseContext, const void *baseAddress) {
    (void)releaseContext;
    free((void *)baseAddress);
}

static CVPixelBufferRef make_bgra(size_t width, size_t height, size_t rowBytes) {
    uint8_t *bytes = malloc(rowBytes * height);
    if (!bytes) return NULL;
    memset(bytes, 0xa5, rowBytes * height);
    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            const TestColor color = kBlockColors[(y / 2) * 4 + (x / 2)];
            uint8_t *pixel = bytes + y * rowBytes + x * 4;
            pixel[0] = color.blue;
            pixel[1] = color.green;
            pixel[2] = color.red;
            pixel[3] = 0xff;
        }
    }

    CVPixelBufferRef pixelBuffer = NULL;
    CVReturn status = CVPixelBufferCreateWithBytes(kCFAllocatorDefault,
        width, height, kCVPixelFormatType_32BGRA, bytes, rowBytes,
        release_bytes, NULL, NULL, &pixelBuffer);
    if (status != kCVReturnSuccess) free(bytes);
    return status == kCVReturnSuccess ? pixelBuffer : NULL;
}

static bool near_value(uint8_t actual, uint8_t expected) {
    return actual + 1 >= expected && actual <= expected + 1;
}

static bool check_bgra_matrix(CFStringRef primaries,
                              CFStringRef expectedMatrixName,
                              const ExpectedMatrix *expected) {
    CVPixelBufferRef source = make_bgra(kTestWidth, kTestHeight, kTestRowBytes);
    if (!source) {
        fprintf(stderr, "could not create padded-stride BGRA source\n");
        return false;
    }
    if (primaries) {
        CVBufferSetAttachment(source, kCVImageBufferColorPrimariesKey,
            primaries, kCVAttachmentMode_ShouldPropagate);
    }
    CVBufferSetAttachment(source, kCVImageBufferTransferFunctionKey,
        kCVImageBufferTransferFunction_ITU_R_709_2,
        kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(source, kCVImageBufferChromaLocationTopFieldKey,
        kCVImageBufferChromaLocation_TopLeft, kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(source, kCVImageBufferChromaLocationBottomFieldKey,
        kCVImageBufferChromaLocation_TopLeft, kCVAttachmentMode_ShouldPropagate);

    CVPixelBufferRef converted = NULL;
    OSStatus status = WEXCopyVideoRangeNV12(source, &converted);
    if (status || !converted) {
        fprintf(stderr, "BGRA conversion failed: %d\n", (int)status);
        CVPixelBufferRelease(source);
        return false;
    }

    bool valid = CVPixelBufferGetPixelFormatType(converted) ==
            kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
        CVPixelBufferGetPlaneCount(converted) == 2 &&
        CVPixelBufferGetWidth(converted) == kTestWidth &&
        CVPixelBufferGetHeight(converted) == kTestHeight &&
        CVPixelBufferGetBytesPerRow(converted) >= kTestWidth;

    CFTypeRef matrix = CVBufferCopyAttachment(converted,
        kCVImageBufferYCbCrMatrixKey, NULL);
    if (!matrix || !CFEqual(matrix, expectedMatrixName)) valid = false;
    if (matrix) CFRelease(matrix);

    WEXColorMetadata metadata;
    status = WEXPackVideoColorMetadata(converted, &metadata);
    uint32_t expectedPrimaries = primaries
        ? (uint32_t)CVColorPrimariesGetIntegerCodePointForString(primaries)
        : WEXColorCodeUnspecified;
    if (status || metadata.reserved != 0 ||
        metadata.colorPrimaries != expectedPrimaries ||
        metadata.transferFunction != (uint32_t)
            CVTransferFunctionGetIntegerCodePointForString(
                kCVImageBufferTransferFunction_ITU_R_709_2) ||
        metadata.yCbCrMatrix != (uint32_t)
            CVYCbCrMatrixGetIntegerCodePointForString(expectedMatrixName) ||
        metadata.chromaLocationTop != WEXChromaCodeCenter ||
        metadata.chromaLocationBottom != WEXChromaCodeCenter) {
        valid = false;
    }
    CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFDictionaryRef ioSurface = CFDictionaryCreate(kCFAllocatorDefault,
        NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CVPixelBufferRef roundTrip = NULL;
    if (attributes && ioSurface) {
        CFDictionarySetValue(attributes, kCVPixelBufferIOSurfacePropertiesKey,
            ioSurface);
        CVReturn createStatus = CVPixelBufferCreate(kCFAllocatorDefault,
            kTestWidth, kTestHeight,
            kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            attributes, &roundTrip);
        if (createStatus != kCVReturnSuccess || !roundTrip ||
            WEXApplyVideoColorMetadata(roundTrip, &metadata) != noErr) {
            valid = false;
        }
    } else {
        valid = false;
    }
    if (roundTrip) {
        CFTypeRef appliedTransfer = CVBufferCopyAttachment(roundTrip,
            kCVImageBufferTransferFunctionKey, NULL);
        CFTypeRef appliedMatrix = CVBufferCopyAttachment(roundTrip,
            kCVImageBufferYCbCrMatrixKey, NULL);
        CFTypeRef appliedTop = CVBufferCopyAttachment(roundTrip,
            kCVImageBufferChromaLocationTopFieldKey, NULL);
        CFTypeRef appliedPrimaries = CVBufferCopyAttachment(roundTrip,
            kCVImageBufferColorPrimariesKey, NULL);
        if (!appliedTransfer || !CFEqual(appliedTransfer,
                kCVImageBufferTransferFunction_ITU_R_709_2) ||
            !appliedMatrix || !CFEqual(appliedMatrix, expectedMatrixName) ||
            !appliedTop || !CFEqual(appliedTop,
                kCVImageBufferChromaLocation_Center) ||
            (primaries && (!appliedPrimaries ||
                !CFEqual(appliedPrimaries, primaries)))) {
            valid = false;
        }
        if (appliedTransfer) CFRelease(appliedTransfer);
        if (appliedMatrix) CFRelease(appliedMatrix);
        if (appliedTop) CFRelease(appliedTop);
        if (appliedPrimaries) CFRelease(appliedPrimaries);
        CVPixelBufferRelease(roundTrip);
    }
    if (ioSurface) CFRelease(ioSurface);
    if (attributes) CFRelease(attributes);

    status = CVPixelBufferLockBaseAddress(converted, kCVPixelBufferLock_ReadOnly);
    if (status != kCVReturnSuccess) valid = false;
    if (valid) {
        const uint8_t *luma = CVPixelBufferGetBaseAddressOfPlane(converted, 0);
        const uint8_t *chroma = CVPixelBufferGetBaseAddressOfPlane(converted, 1);
        size_t lumaStride = CVPixelBufferGetBytesPerRowOfPlane(converted, 0);
        size_t chromaStride = CVPixelBufferGetBytesPerRowOfPlane(converted, 1);
        for (size_t block = 0; block < 8; ++block) {
            size_t x = (block % 4) * 2;
            size_t y = (block / 4) * 2;
            const uint8_t actualY = luma[y * lumaStride + x];
            const uint8_t actualCb = chroma[(y / 2) * chromaStride + x];
            const uint8_t actualCr = chroma[(y / 2) * chromaStride + x + 1];
            if (!near_value(actualY, expected->y[block]) ||
                !near_value(actualCb, expected->cb[block]) ||
                !near_value(actualCr, expected->cr[block])) {
                fprintf(stderr,
                    "matrix sample %zu mismatch: Y %u/%u Cb %u/%u Cr %u/%u\n",
                    block, actualY, expected->y[block],
                    actualCb, expected->cb[block], actualCr, expected->cr[block]);
                valid = false;
            }
        }
        if (lumaStride < kTestWidth || chromaStride < kTestWidth) valid = false;
    }
    if (status == kCVReturnSuccess) {
        CVPixelBufferUnlockBaseAddress(converted, kCVPixelBufferLock_ReadOnly);
    }
    CVPixelBufferRelease(converted);
    CVPixelBufferRelease(source);
    return valid;
}

static bool check_nv12_retain(void) {
    CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (!attributes) return false;
    CFDictionaryRef ioSurface = CFDictionaryCreate(kCFAllocatorDefault,
        NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (!ioSurface) {
        CFRelease(attributes);
        return false;
    }
    CFDictionarySetValue(attributes, kCVPixelBufferIOSurfacePropertiesKey, ioSurface);
    CFRelease(ioSurface);

    CVPixelBufferRef source = NULL;
    CVReturn createStatus = CVPixelBufferCreate(kCFAllocatorDefault, 4, 4,
        kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attributes, &source);
    CFRelease(attributes);
    if (createStatus != kCVReturnSuccess || !source) return false;

    CVPixelBufferRef result = NULL;
    OSStatus status = WEXCopyVideoRangeNV12(source, &result);
    bool valid = status == noErr && result == source;
    CVPixelBufferRelease(source);
    if (valid) valid = CVPixelBufferGetPixelFormatType(result) ==
        kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    if (result) CVPixelBufferRelease(result);
    return valid;
}

static bool check_unmarked_metadata_stays_unmarked(void) {
    CVPixelBufferRef source = make_bgra(kTestWidth, kTestHeight, kTestRowBytes);
    if (!source) return false;
    CVPixelBufferRef converted = NULL;
    OSStatus status = WEXCopyVideoRangeNV12(source, &converted);
    CVPixelBufferRelease(source);
    if (status || !converted) return false;

    WEXColorMetadata metadata;
    status = WEXPackVideoColorMetadata(converted, &metadata);
    bool valid = status == noErr &&
        metadata.colorPrimaries == WEXColorCodeUnspecified &&
        metadata.transferFunction == WEXColorCodeUnspecified &&
        metadata.yCbCrMatrix == WEXColorCodeUnspecified &&
        metadata.chromaLocationTop == WEXColorCodeUnspecified &&
        metadata.chromaLocationBottom == WEXColorCodeUnspecified;
    CVPixelBufferRelease(converted);
    return valid;
}

static bool check_rejects_unsupported_inputs(void) {
    CVPixelBufferRef oddWidth = make_bgra(3, 4, 16);
    if (!oddWidth) return false;
    CVPixelBufferRef result = NULL;
    OSStatus oddStatus = WEXCopyVideoRangeNV12(oddWidth, &result);
    CVPixelBufferRelease(oddWidth);
    if (oddStatus == noErr || result) return false;

    CVPixelBufferRef unsupported = NULL;
    CVReturn createStatus = CVPixelBufferCreate(kCFAllocatorDefault, 4, 4,
        kCVPixelFormatType_32ARGB, NULL, &unsupported);
    if (createStatus != kCVReturnSuccess || !unsupported) return false;
    OSStatus unsupportedStatus = WEXCopyVideoRangeNV12(unsupported, &result);
    CVPixelBufferRelease(unsupported);
    if (result) CVPixelBufferRelease(result);
    return unsupportedStatus == kCVReturnInvalidPixelFormat;
}

int main(void) {
    bool rec709 = check_bgra_matrix(kCVImageBufferColorPrimaries_ITU_R_709_2,
        kCVImageBufferYCbCrMatrix_ITU_R_709_2, &kRec709);
    bool rec601 = check_bgra_matrix(kCVImageBufferColorPrimaries_SMPTE_C,
        kCVImageBufferYCbCrMatrix_ITU_R_601_4, &kRec601);
    bool rec2020 = check_bgra_matrix(kCVImageBufferColorPrimaries_ITU_R_2020,
        kCVImageBufferYCbCrMatrix_ITU_R_2020, &kRec2020);
    bool unmarked = check_unmarked_metadata_stays_unmarked();
    bool nv12 = check_nv12_retain();
    bool rejects = check_rejects_unsupported_inputs();
    printf("BT.709=%s BT.601=%s BT.2020=%s unmarked=%s NV12-retain=%s reject=%s\n",
        rec709 ? "PASS" : "FAIL", rec601 ? "PASS" : "FAIL",
        rec2020 ? "PASS" : "FAIL", unmarked ? "PASS" : "FAIL",
        nv12 ? "PASS" : "FAIL",
        rejects ? "PASS" : "FAIL");
    return rec709 && rec601 && rec2020 && unmarked && nv12 && rejects ? 0 : 1;
}
