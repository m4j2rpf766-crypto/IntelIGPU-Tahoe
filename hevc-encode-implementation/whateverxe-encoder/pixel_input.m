#include "pixel_input.h"

#include <Accelerate/Accelerate.h>
#include <CoreVideo/CVImageBuffer.h>
#include <limits.h>

enum {
    WEXPixelInputMaximumWidth = 1920,
    WEXPixelInputMaximumHeight = 1080
};

static bool matrix_for_source(CVPixelBufferRef source,
                              const vImage_ARGBToYpCbCrMatrix **matrixOut,
                              CFStringRef *matrixNameOut) {
    CFTypeRef matrixAttachment = CVBufferCopyAttachment(source,
        kCVImageBufferYCbCrMatrixKey, NULL);
    CFTypeRef primariesAttachment = CVBufferCopyAttachment(source,
        kCVImageBufferColorPrimariesKey, NULL);
    CFStringRef matrixName = NULL;
    const vImage_ARGBToYpCbCrMatrix *matrix = NULL;

    if (matrixAttachment && CFGetTypeID(matrixAttachment) != CFStringGetTypeID()) {
        CFRelease(matrixAttachment);
        if (primariesAttachment) CFRelease(primariesAttachment);
        return false;
    }
    if (primariesAttachment &&
        CFGetTypeID(primariesAttachment) != CFStringGetTypeID()) {
        if (matrixAttachment) CFRelease(matrixAttachment);
        CFRelease(primariesAttachment);
        return false;
    }

    CFStringRef sourceMatrix = (CFStringRef)matrixAttachment;
    CFStringRef primaries = (CFStringRef)primariesAttachment;
    if (sourceMatrix && CFEqual(sourceMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2)) {
        matrix = kvImage_ARGBToYpCbCrMatrix_ITU_R_709_2;
        matrixName = kCVImageBufferYCbCrMatrix_ITU_R_709_2;
    } else if (sourceMatrix &&
               CFEqual(sourceMatrix, kCVImageBufferYCbCrMatrix_ITU_R_601_4)) {
        matrix = kvImage_ARGBToYpCbCrMatrix_ITU_R_601_4;
        matrixName = kCVImageBufferYCbCrMatrix_ITU_R_601_4;
    } else if (sourceMatrix &&
               CFEqual(sourceMatrix, kCVImageBufferYCbCrMatrix_ITU_R_2020)) {
        static const vImage_ARGBToYpCbCrMatrix rec2020 = {
            .R_Yp = 0.2627f,
            .G_Yp = 0.6780f,
            .B_Yp = 0.0593f,
            .R_Cb = -0.13963f,
            .G_Cb = -0.36037f,
            .B_Cb_R_Cr = 0.5f,
            .G_Cr = -0.459786f,
            .B_Cr = -0.040214f
        };
        matrix = &rec2020;
        matrixName = kCVImageBufferYCbCrMatrix_ITU_R_2020;
    } else if (sourceMatrix) {
        if (matrixAttachment) CFRelease(matrixAttachment);
        if (primariesAttachment) CFRelease(primariesAttachment);
        return false;
    } else if (primaries && CFEqual(primaries,
                                    kCVImageBufferColorPrimaries_ITU_R_709_2)) {
        matrix = kvImage_ARGBToYpCbCrMatrix_ITU_R_709_2;
        matrixName = kCVImageBufferYCbCrMatrix_ITU_R_709_2;
    } else if (primaries && CFEqual(primaries,
                                    kCVImageBufferColorPrimaries_ITU_R_2020)) {
        static const vImage_ARGBToYpCbCrMatrix rec2020 = {
            .R_Yp = 0.2627f,
            .G_Yp = 0.6780f,
            .B_Yp = 0.0593f,
            .R_Cb = -0.13963f,
            .G_Cb = -0.36037f,
            .B_Cb_R_Cr = 0.5f,
            .G_Cr = -0.459786f,
            .B_Cr = -0.040214f
        };
        matrix = &rec2020;
        matrixName = kCVImageBufferYCbCrMatrix_ITU_R_2020;
    } else if (primaries && (CFEqual(primaries,
                                     kCVImageBufferColorPrimaries_EBU_3213) ||
                             CFEqual(primaries,
                                     kCVImageBufferColorPrimaries_SMPTE_C))) {
        matrix = kvImage_ARGBToYpCbCrMatrix_ITU_R_601_4;
        matrixName = kCVImageBufferYCbCrMatrix_ITU_R_601_4;
    } else if (!primaries) {
        // HD BGRA without color tags is converted using the common 709 matrix.
        // Keep the wire attachments unmarked because the input did not state 709.
        matrix = kvImage_ARGBToYpCbCrMatrix_ITU_R_709_2;
    } else {
        if (matrixAttachment) CFRelease(matrixAttachment);
        CFRelease(primariesAttachment);
        return false;
    }

    if (matrixAttachment) CFRelease(matrixAttachment);
    if (primariesAttachment) CFRelease(primariesAttachment);
    *matrixOut = matrix;
    *matrixNameOut = matrixName;
    return true;
}

static bool source_has_chroma_attachment(CVPixelBufferRef source) {
    CFTypeRef top = CVBufferCopyAttachment(source,
        kCVImageBufferChromaLocationTopFieldKey, NULL);
    CFTypeRef bottom = CVBufferCopyAttachment(source,
        kCVImageBufferChromaLocationBottomFieldKey, NULL);
    bool hasAttachment = top || bottom;
    if (top) CFRelease(top);
    if (bottom) CFRelease(bottom);
    return hasAttachment;
}

static bool valid_dimensions(CVPixelBufferRef pixelBuffer) {
    size_t width = CVPixelBufferGetWidth(pixelBuffer);
    size_t height = CVPixelBufferGetHeight(pixelBuffer);
    return width >= 2 && height >= 2 &&
        width <= WEXPixelInputMaximumWidth &&
        height <= WEXPixelInputMaximumHeight &&
        (width & 1) == 0 && (height & 1) == 0;
}

static OSStatus copy_bgra_to_nv12(CVPixelBufferRef source,
                                  CVPixelBufferRef *pixelBufferOut) {
    const vImage_ARGBToYpCbCrMatrix *matrix = NULL;
    CFStringRef yCbCrMatrix = NULL;
    if (!matrix_for_source(source, &matrix, &yCbCrMatrix))
        return kCVReturnInvalidPixelFormat;
    bool updateChromaLocation = source_has_chroma_attachment(source);

    size_t width = CVPixelBufferGetWidth(source);
    size_t height = CVPixelBufferGetHeight(source);
    size_t sourceRowBytes = CVPixelBufferGetBytesPerRow(source);
    if (width > SIZE_MAX / 4 || sourceRowBytes < width * 4) {
        return kCVReturnInvalidArgument;
    }

    CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (!attributes) return kCVReturnAllocationFailed;

    CFDictionaryRef ioSurface = CFDictionaryCreate(kCFAllocatorDefault,
        NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (!ioSurface) {
        CFRelease(attributes);
        return kCVReturnAllocationFailed;
    }
    CFDictionarySetValue(attributes, kCVPixelBufferIOSurfacePropertiesKey, ioSurface);
    CFRelease(ioSurface);

    CVPixelBufferRef destination = NULL;
    CVReturn cvStatus = CVPixelBufferCreate(kCFAllocatorDefault, width, height,
        kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attributes, &destination);
    CFRelease(attributes);
    if (cvStatus != kCVReturnSuccess) return cvStatus;

    cvStatus = CVPixelBufferLockBaseAddress(source, kCVPixelBufferLock_ReadOnly);
    if (cvStatus != kCVReturnSuccess) {
        CVPixelBufferRelease(destination);
        return cvStatus;
    }
    cvStatus = CVPixelBufferLockBaseAddress(destination, 0);
    if (cvStatus != kCVReturnSuccess) {
        CVPixelBufferUnlockBaseAddress(source, kCVPixelBufferLock_ReadOnly);
        CVPixelBufferRelease(destination);
        return cvStatus;
    }

    void *sourceBase = CVPixelBufferGetBaseAddress(source);
    void *lumaBase = CVPixelBufferGetBaseAddressOfPlane(destination, 0);
    void *chromaBase = CVPixelBufferGetBaseAddressOfPlane(destination, 1);
    size_t lumaRowBytes = CVPixelBufferGetBytesPerRowOfPlane(destination, 0);
    size_t chromaRowBytes = CVPixelBufferGetBytesPerRowOfPlane(destination, 1);
    if (!sourceBase || !lumaBase || !chromaBase ||
        CVPixelBufferGetPlaneCount(destination) != 2 ||
        lumaRowBytes < width || chromaRowBytes < width) {
        cvStatus = kCVReturnInvalidArgument;
    } else {
        vImage_Buffer sourceImage = {
            .data = sourceBase,
            .height = height,
            .width = width,
            .rowBytes = sourceRowBytes
        };
        vImage_Buffer luma = {
            .data = lumaBase,
            .height = height,
            .width = width,
            .rowBytes = lumaRowBytes
        };
        vImage_Buffer chroma = {
            .data = chromaBase,
            .height = height / 2,
            .width = width / 2,
            .rowBytes = chromaRowBytes
        };
        const vImage_YpCbCrPixelRange videoRange = {
            .Yp_bias = 16,
            .CbCr_bias = 128,
            .YpRangeMax = 235,
            .CbCrRangeMax = 240,
            .YpMax = 235,
            .YpMin = 16,
            .CbCrMax = 240,
            .CbCrMin = 16
        };
        vImage_ARGBToYpCbCr conversion;
        vImage_Error imageStatus = vImageConvert_ARGBToYpCbCr_GenerateConversion(
            matrix, &videoRange, &conversion, kvImageARGB8888,
            kvImage420Yp8_CbCr8, kvImageNoFlags);
        if (imageStatus == kvImageNoError) {
            const uint8_t bgraPermutation[4] = {3, 2, 1, 0};
            imageStatus = vImageConvert_ARGB8888To420Yp8_CbCr8(
                &sourceImage, &luma, &chroma, &conversion,
                bgraPermutation, kvImageNoFlags);
        }
        if (imageStatus != kvImageNoError) cvStatus = kCVReturnInvalidArgument;
    }

    CVPixelBufferUnlockBaseAddress(destination, 0);
    CVPixelBufferUnlockBaseAddress(source, kCVPixelBufferLock_ReadOnly);
    if (cvStatus != kCVReturnSuccess) {
        CVPixelBufferRelease(destination);
        return cvStatus;
    }

    CVBufferPropagateAttachments(source, destination);
    if (yCbCrMatrix) {
        CVBufferSetAttachment(destination, kCVImageBufferYCbCrMatrixKey,
            yCbCrMatrix, kCVAttachmentMode_ShouldPropagate);
    }
    if (updateChromaLocation) {
        CVBufferSetAttachment(destination, kCVImageBufferChromaLocationTopFieldKey,
            kCVImageBufferChromaLocation_Center, kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(destination, kCVImageBufferChromaLocationBottomFieldKey,
            kCVImageBufferChromaLocation_Center, kCVAttachmentMode_ShouldPropagate);
    }

    *pixelBufferOut = destination;
    return noErr;
}

OSStatus WEXCopyVideoRangeNV12(CVPixelBufferRef source,
                               CVPixelBufferRef *pixelBufferOut) {
    if (!source || !pixelBufferOut) return kCVReturnInvalidArgument;
    *pixelBufferOut = NULL;
    if (!valid_dimensions(source)) return kCVReturnInvalidArgument;

    OSType pixelFormat = CVPixelBufferGetPixelFormatType(source);
    if (pixelFormat == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
        CVPixelBufferGetPlaneCount(source) == 2) {
        CVPixelBufferRetain(source);
        *pixelBufferOut = source;
        return noErr;
    }
    if (pixelFormat != kCVPixelFormatType_32BGRA ||
        CVPixelBufferIsPlanar(source)) {
        return kCVReturnInvalidPixelFormat;
    }
    return copy_bgra_to_nv12(source, pixelBufferOut);
}

static uint32_t attachment_codepoint(CVBufferRef buffer, CFStringRef key,
                                     int (*convert)(CFStringRef)) {
    CFTypeRef value = CVBufferCopyAttachment(buffer, key, NULL);
    if (!value) return WEXColorCodeUnspecified;
    uint32_t result = WEXColorCodeUnspecified;
    if (CFGetTypeID(value) == CFStringGetTypeID()) {
        int codepoint = convert((CFStringRef)value);
        if (codepoint >= 0 && codepoint != 2) result = (uint32_t)codepoint;
    }
    CFRelease(value);
    return result;
}

static uint32_t chroma_codepoint(CVBufferRef buffer, CFStringRef key) {
    CFTypeRef value = CVBufferCopyAttachment(buffer, key, NULL);
    if (!value) return WEXColorCodeUnspecified;

    uint32_t result = WEXColorCodeUnspecified;
    if (CFGetTypeID(value) == CFStringGetTypeID()) {
        if (CFEqual(value, kCVImageBufferChromaLocation_Center))
            result = WEXChromaCodeCenter;
        else if (CFEqual(value, kCVImageBufferChromaLocation_Left))
            result = WEXChromaCodeLeft;
        else if (CFEqual(value, kCVImageBufferChromaLocation_TopLeft))
            result = WEXChromaCodeTopLeft;
        else if (CFEqual(value, kCVImageBufferChromaLocation_Top))
            result = WEXChromaCodeTop;
        else if (CFEqual(value, kCVImageBufferChromaLocation_BottomLeft))
            result = WEXChromaCodeBottomLeft;
        else if (CFEqual(value, kCVImageBufferChromaLocation_Bottom))
            result = WEXChromaCodeBottom;
        else if (CFEqual(value, kCVImageBufferChromaLocation_DV420))
            result = WEXChromaCodeDV420;
    }
    CFRelease(value);
    return result;
}

OSStatus WEXPackVideoColorMetadata(CVPixelBufferRef source,
                                   WEXColorMetadata *metadata) {
    if (!source || !metadata) return kCVReturnInvalidArgument;

    *metadata = (WEXColorMetadata) {
        .colorPrimaries = WEXColorCodeUnspecified,
        .transferFunction = WEXColorCodeUnspecified,
        .yCbCrMatrix = WEXColorCodeUnspecified,
        .chromaLocationTop = WEXColorCodeUnspecified,
        .chromaLocationBottom = WEXColorCodeUnspecified,
        .reserved = 0
    };
    metadata->colorPrimaries = attachment_codepoint(source,
        kCVImageBufferColorPrimariesKey,
        CVColorPrimariesGetIntegerCodePointForString);
    metadata->transferFunction = attachment_codepoint(source,
        kCVImageBufferTransferFunctionKey,
        CVTransferFunctionGetIntegerCodePointForString);
    metadata->yCbCrMatrix = attachment_codepoint(source,
        kCVImageBufferYCbCrMatrixKey,
        CVYCbCrMatrixGetIntegerCodePointForString);
    metadata->chromaLocationTop = chroma_codepoint(source,
        kCVImageBufferChromaLocationTopFieldKey);
    metadata->chromaLocationBottom = chroma_codepoint(source,
        kCVImageBufferChromaLocationBottomFieldKey);
    return noErr;
}

static CFStringRef chroma_string(uint32_t codepoint) {
    switch (codepoint) {
        case WEXChromaCodeCenter: return kCVImageBufferChromaLocation_Center;
        case WEXChromaCodeLeft: return kCVImageBufferChromaLocation_Left;
        case WEXChromaCodeTopLeft: return kCVImageBufferChromaLocation_TopLeft;
        case WEXChromaCodeTop: return kCVImageBufferChromaLocation_Top;
        case WEXChromaCodeBottomLeft: return kCVImageBufferChromaLocation_BottomLeft;
        case WEXChromaCodeBottom: return kCVImageBufferChromaLocation_Bottom;
        case WEXChromaCodeDV420: return kCVImageBufferChromaLocation_DV420;
        default: return NULL;
    }
}

static OSStatus apply_string_attachment(CVPixelBufferRef buffer, CFStringRef key,
                                        uint32_t codepoint,
                                        CFStringRef (*convert)(int)) {
    if (codepoint == WEXColorCodeUnspecified) return noErr;
    if (codepoint > INT_MAX) return kCVReturnInvalidArgument;
    CFStringRef value = convert((int)codepoint);
    if (!value) return kCVReturnInvalidArgument;
    CVBufferSetAttachment(buffer, key, value, kCVAttachmentMode_ShouldPropagate);
    return noErr;
}

OSStatus WEXApplyVideoColorMetadata(CVPixelBufferRef destination,
                                    const WEXColorMetadata *metadata) {
    if (!destination || !metadata || metadata->reserved != 0)
        return kCVReturnInvalidArgument;

    OSStatus status = apply_string_attachment(destination,
        kCVImageBufferColorPrimariesKey, metadata->colorPrimaries,
        CVColorPrimariesGetStringForIntegerCodePoint);
    if (!status) {
        status = apply_string_attachment(destination,
            kCVImageBufferTransferFunctionKey, metadata->transferFunction,
            CVTransferFunctionGetStringForIntegerCodePoint);
    }
    if (!status) {
        status = apply_string_attachment(destination,
            kCVImageBufferYCbCrMatrixKey, metadata->yCbCrMatrix,
            CVYCbCrMatrixGetStringForIntegerCodePoint);
    }
    if (status) return status;

    if (metadata->chromaLocationTop != WEXColorCodeUnspecified) {
        CFStringRef value = chroma_string(metadata->chromaLocationTop);
        if (!value) return kCVReturnInvalidArgument;
        CVBufferSetAttachment(destination, kCVImageBufferChromaLocationTopFieldKey,
            value, kCVAttachmentMode_ShouldPropagate);
    }
    if (metadata->chromaLocationBottom != WEXColorCodeUnspecified) {
        CFStringRef value = chroma_string(metadata->chromaLocationBottom);
        if (!value) return kCVReturnInvalidArgument;
        CVBufferSetAttachment(destination, kCVImageBufferChromaLocationBottomFieldKey,
            value, kCVAttachmentMode_ShouldPropagate);
    }
    return noErr;
}
