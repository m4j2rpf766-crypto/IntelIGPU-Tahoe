#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#include <VideoToolbox/VideoToolbox.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "native_backend.h"
#include "pixel_input.h"

typedef struct {
    WEXConfig config;
    WEXProbeInfo probe;
    void *nativeSession;
    uint64_t frameCount;
    uint8_t *parameterSets[3];
    size_t parameterSetBytes[3];
    int nalLengthBytes;
} WEXWorker;

static int read_exact(int fd, void *buffer, size_t length, int *cleanEOF) {
    uint8_t *cursor = buffer;
    size_t received = 0;
    if (cleanEOF) *cleanEOF = 0;
    while (received < length) {
        ssize_t amount = read(fd, cursor + received, length - received);
        if (amount == 0) {
            if (received == 0 && cleanEOF) {
                *cleanEOF = 1;
                return 0;
            }
            return -1;
        }
        if (amount < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        received += (size_t)amount;
    }
    return 0;
}

static int write_exact(int fd, const void *buffer, size_t length) {
    const uint8_t *cursor = buffer;
    size_t sent = 0;
    while (sent < length) {
        ssize_t amount = write(fd, cursor + sent, length - sent);
        if (amount < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (amount == 0) return -1;
        sent += (size_t)amount;
    }
    return 0;
}

static int send_reply(uint16_t kind, OSStatus status, const void *payload, uint32_t payloadBytes) {
    WEXMessage reply = {
        .magic = WEXWireMagic,
        .version = WEXWireVersion,
        .kind = kind,
        .payloadBytes = payloadBytes,
        .status = status
    };
    if (write_exact(STDIN_FILENO, &reply, sizeof(reply)) != 0) return -1;
    if (payloadBytes && write_exact(STDIN_FILENO, payload, payloadBytes) != 0) return -1;
    return 0;
}

static int copy_registry_u32(io_registry_entry_t entry, CFStringRef key, uint32_t *value) {
    CFTypeRef property = IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0);
    if (!property) return 0;
    int found = 0;
    if (CFGetTypeID(property) == CFDataGetTypeID()) {
        CFDataRef data = (CFDataRef)property;
        if (CFDataGetLength(data) >= 4) {
            const UInt8 *bytes = CFDataGetBytePtr(data);
            *value = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
                ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
            found = 1;
        }
    } else if (CFGetTypeID(property) == CFNumberGetTypeID()) {
        int number = 0;
        if (CFNumberGetValue((CFNumberRef)property, kCFNumberIntType, &number)) {
            *value = (uint32_t)number;
            found = 1;
        }
    }
    CFRelease(property);
    return found;
}

static int parent_is_qualified_pci_device(io_registry_entry_t accelerator) {
    io_registry_entry_t current = accelerator;
    if (IOObjectRetain(current) != KERN_SUCCESS) return 0;
    for (unsigned depth = 0; depth < 12 && current != IO_OBJECT_NULL; ++depth) {
        uint32_t vendor = 0, device = 0;
        if (IOObjectConformsTo(current, "IOPCIDevice") &&
            copy_registry_u32(current, CFSTR("vendor-id"), &vendor) &&
            copy_registry_u32(current, CFSTR("device-id"), &device)) {
            int qualified = vendor == WEXIntelVendorID && device == WEXUHD770DeviceID;
            IOObjectRelease(current);
            return qualified;
        }
        io_registry_entry_t parent = IO_OBJECT_NULL;
        if (IORegistryEntryGetParentEntry(current, kIOServicePlane, &parent) != KERN_SUCCESS) {
            IOObjectRelease(current);
            return 0;
        }
        IOObjectRelease(current);
        current = parent;
    }
    if (current != IO_OBJECT_NULL) IOObjectRelease(current);
    return 0;
}

static OSStatus verify_qualified_intel_accelerator(uint64_t registryID) {
    if (registryID == 0) return paramErr;
    CFMutableDictionaryRef matching = IORegistryEntryIDMatching(registryID);
    if (!matching) return kVTVideoEncoderNotAvailableNowErr;

    io_service_t accelerator = IOServiceGetMatchingService(kIOMainPortDefault, matching);
    if (accelerator == IO_OBJECT_NULL) return kVTVideoEncoderNotAvailableNowErr;

    uint64_t actualRegistryID = 0;
    kern_return_t status = IORegistryEntryGetRegistryEntryID(accelerator, &actualRegistryID);
    int matches = IOObjectConformsTo(accelerator, "IntelAccelerator") &&
        status == KERN_SUCCESS && actualRegistryID == registryID &&
        parent_is_qualified_pci_device(accelerator);
    IOObjectRelease(accelerator);
    return matches ? noErr : kVTVideoEncoderNotAvailableNowErr;
}

static OSStatus validate_native_probe(const WEXProbeInfo *info, uint64_t physicalRegistryID) {
    if (!info || info->abiVersion != WEXNativeProbeABIVersion ||
        info->structBytes != sizeof(WEXProbeInfo) || info->reserved != 0 ||
        info->vendorID != WEXIntelVendorID || info->deviceID != WEXUHD770DeviceID ||
        info->registryID != physicalRegistryID ||
        (info->capabilities & WEXProbeRequiredCapabilities) != WEXProbeRequiredCapabilities ||
        info->maxWidth == 0 || info->maxHeight == 0 ||
        (info->maxWidth & 1) || (info->maxHeight & 1))
        return kVTVideoEncoderNotAvailableNowErr;
    return noErr;
}

static OSStatus probe_native_backend(WEXProbeInfo *result, uint64_t expectedRegistryID) {
    WEXProbeInfo info = {0};
    OSStatus status = WEXNativeHEVCProbe(&info);
    if (status != noErr) return status;

    uint64_t physicalRegistryID = expectedRegistryID ? expectedRegistryID : info.registryID;
    status = validate_native_probe(&info, physicalRegistryID);
    if (status != noErr) {
        fprintf(stderr, "WEX_WORKER native adapter probe did not match physical 8086:4680 device\n");
        return status;
    }
    status = verify_qualified_intel_accelerator(physicalRegistryID);
    if (status != noErr) {
        fprintf(stderr, "WEX_WORKER registry id 0x%llx is not the active UHD 770 accelerator\n",
            (unsigned long long)physicalRegistryID);
        return status;
    }
    if (result) *result = info;
    return noErr;
}

static int time_from_wire(int64_t value, int32_t scale, CMTime *time) {
    if (scale == 0) {
        *time = kCMTimeInvalid;
        return 1;
    }
    if (scale < 0) return 0;
    *time = CMTimeMake(value, scale);
    return CMTIME_IS_VALID(*time);
}

static void time_to_wire(CMTime time, int64_t *value, int32_t *scale) {
    if (!CMTIME_IS_NUMERIC(time) || time.timescale <= 0) {
        *value = 0;
        *scale = 0;
        return;
    }
    *value = time.value;
    *scale = time.timescale;
}

static OSStatus validate_config(const WEXConfig *config, const WEXProbeInfo *probe) {
    if (!config || !probe || config->registryID == 0 ||
        config->registryID != probe->registryID ||
        config->width <= 0 || config->height <= 0 ||
        config->width > WEXMaxWidth || config->height > WEXMaxHeight ||
        config->width > (int32_t)probe->maxWidth || config->height > (int32_t)probe->maxHeight ||
        (config->width & 1) || (config->height & 1) ||
        config->bitrate < 64000 || config->bitrate > 100000000 ||
        config->fpsNum <= 0 || config->fpsDen <= 0 ||
        (double)config->fpsNum / config->fpsDen > 240.0)
        return paramErr;
    return noErr;
}

static OSStatus open_native_session(WEXWorker *worker) {
    OSStatus status = verify_qualified_intel_accelerator(worker->config.registryID);
    if (status != noErr) return status;
    status = probe_native_backend(&worker->probe, worker->config.registryID);
    if (status != noErr) return status;
    status = validate_config(&worker->config, &worker->probe);
    if (status != noErr) return status;
    status = WEXNativeHEVCOpen(&worker->config, &worker->probe, &worker->nativeSession);
    if (status != noErr) {
        fprintf(stderr, "WEX_WORKER native HEVC open failed status=%d\n", status);
        return status;
    }
    if (!worker->nativeSession) return kVTVideoEncoderNotAvailableNowErr;
    fprintf(stderr, "WEX_WORKER native backend ready GPU=0x%llx size=%dx%d\n",
        (unsigned long long)worker->probe.registryID, worker->config.width, worker->config.height);
    return noErr;
}

static OSStatus create_input_pixel_buffer(WEXWorker *worker, const uint8_t *nv12,
    CVPixelBufferRef *pixelBuffer) {
    NSDictionary *attributes = @{
        (__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey: @{},
        (__bridge NSString *)kCVPixelBufferMetalCompatibilityKey: @YES
    };
    OSStatus status = CVPixelBufferCreate(kCFAllocatorDefault,
        (size_t)worker->config.width, (size_t)worker->config.height,
        kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
        (__bridge CFDictionaryRef)attributes, pixelBuffer);
    if (status != kCVReturnSuccess) return status;

    status = CVPixelBufferLockBaseAddress(*pixelBuffer, 0);
    if (status != kCVReturnSuccess) {
        CVPixelBufferRelease(*pixelBuffer);
        *pixelBuffer = NULL;
        return status;
    }

    size_t width = (size_t)worker->config.width;
    size_t height = (size_t)worker->config.height;
    uint8_t *yPlane = CVPixelBufferGetBaseAddressOfPlane(*pixelBuffer, 0);
    uint8_t *uvPlane = CVPixelBufferGetBaseAddressOfPlane(*pixelBuffer, 1);
    size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(*pixelBuffer, 0);
    size_t uvStride = CVPixelBufferGetBytesPerRowOfPlane(*pixelBuffer, 1);
    size_t yRows = CVPixelBufferGetHeightOfPlane(*pixelBuffer, 0);
    size_t uvRows = CVPixelBufferGetHeightOfPlane(*pixelBuffer, 1);
    if (!yPlane || !uvPlane || yStride < width || uvStride < width ||
        yRows != height || uvRows != height / 2) {
        CVPixelBufferUnlockBaseAddress(*pixelBuffer, 0);
        CVPixelBufferRelease(*pixelBuffer);
        *pixelBuffer = NULL;
        return kCVReturnInvalidPixelBufferAttributes;
    }

    const uint8_t *uvSource = nv12 + width * height;
    for (size_t row = 0; row < height; ++row)
        memcpy(yPlane + row * yStride, nv12 + row * width, width);
    for (size_t row = 0; row < height / 2; ++row)
        memcpy(uvPlane + row * uvStride, uvSource + row * width, width);

    status = CVPixelBufferUnlockBaseAddress(*pixelBuffer, 0);
    if (status != kCVReturnSuccess) {
        CVPixelBufferRelease(*pixelBuffer);
        *pixelBuffer = NULL;
    }
    return status;
}

static int parameter_set_slot(const uint8_t *bytes, size_t length) {
    if (!bytes || length < 2) return -1;
    switch ((bytes[0] >> 1) & 0x3f) {
        case 32: return 0; /* VPS */
        case 33: return 1; /* SPS */
        case 34: return 2; /* PPS */
        default: return -1;
    }
}

static int cached_parameter_sets_complete(const WEXWorker *worker) {
    return worker->parameterSets[0] && worker->parameterSets[1] && worker->parameterSets[2];
}

static OSStatus update_parameter_sets(WEXWorker *worker, CMFormatDescriptionRef format) {
    if (!format || CMFormatDescriptionGetMediaSubType(format) != kCMVideoCodecType_HEVC)
        return kVTVideoEncoderMalfunctionErr;

    const uint8_t *bytes = NULL;
    size_t length = 0;
    size_t count = 0;
    int nalLengthBytes = 0;
    OSStatus status = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, 0,
        &bytes, &length, &count, &nalLengthBytes);
    if (status != noErr) return cached_parameter_sets_complete(worker) ? noErr : status;
    if (nalLengthBytes < 1 || nalLengthBytes > 4 || count > 64)
        return kVTVideoEncoderMalfunctionErr;

    uint8_t *newSets[3] = {NULL, NULL, NULL};
    size_t newLengths[3] = {0, 0, 0};
    for (size_t index = 0; index < count; ++index) {
        status = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, index,
            &bytes, &length, NULL, NULL);
        if (status != noErr) break;
        int slot = parameter_set_slot(bytes, length);
        if (slot < 0 || newSets[slot]) continue;
        if (length > WEXMaxParameterSet) {
            status = kVTVideoEncoderMalfunctionErr;
            break;
        }
        newSets[slot] = malloc(length);
        if (!newSets[slot]) {
            status = memFullErr;
            break;
        }
        memcpy(newSets[slot], bytes, length);
        newLengths[slot] = length;
    }

    if (status == noErr) {
        int complete = 1;
        for (unsigned slot = 0; slot < 3; ++slot)
            if (!newSets[slot] && !worker->parameterSets[slot]) complete = 0;
        if (!complete) status = kVTVideoEncoderMalfunctionErr;
    }
    if (status == noErr) {
        for (unsigned slot = 0; slot < 3; ++slot) {
            if (!newSets[slot]) continue;
            free(worker->parameterSets[slot]);
            worker->parameterSets[slot] = newSets[slot];
            worker->parameterSetBytes[slot] = newLengths[slot];
            newSets[slot] = NULL;
        }
        worker->nalLengthBytes = nalLengthBytes;
    }
    for (unsigned slot = 0; slot < 3; ++slot) free(newSets[slot]);
    return status;
}

static int sample_is_key_frame(CMSampleBufferRef sampleBuffer) {
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
    if (!attachments || CFArrayGetCount(attachments) == 0) return 1;
    CFTypeRef first = CFArrayGetValueAtIndex(attachments, 0);
    if (!first || CFGetTypeID(first) != CFDictionaryGetTypeID()) return 1;
    CFTypeRef notSync = CFDictionaryGetValue((CFDictionaryRef)first,
        kCMSampleAttachmentKey_NotSync);
    return !notSync || notSync != kCFBooleanTrue;
}

static OSStatus validate_length_prefixed_access_unit(CMBlockBufferRef block, int lengthBytes,
    size_t *codedBytes) {
    if (!block || lengthBytes < 1 || lengthBytes > 4) return kVTVideoEncoderMalfunctionErr;
    size_t total = CMBlockBufferGetDataLength(block);
    if (total == 0 || total > WEXMaxPacket) return kVTVideoEncoderMalfunctionErr;
    uint8_t *bytes = malloc(total);
    if (!bytes) return memFullErr;
    OSStatus status = CMBlockBufferCopyDataBytes(block, 0, total, bytes);
    if (status == noErr) {
        size_t offset = 0;
        while (offset < total) {
            if (total - offset < (size_t)lengthBytes) {
                status = kVTVideoEncoderMalfunctionErr;
                break;
            }
            uint32_t nalLength = 0;
            for (int index = 0; index < lengthBytes; ++index)
                nalLength = (nalLength << 8) | bytes[offset++];
            if (nalLength == 0 || nalLength > total - offset) {
                status = kVTVideoEncoderMalfunctionErr;
                break;
            }
            offset += nalLength;
        }
    }
    free(bytes);
    if (status == noErr) *codedBytes = total;
    return status;
}

static OSStatus pack_sample(WEXWorker *worker, CMSampleBufferRef sampleBuffer,
    WEXEncodedInfo *info, uint8_t **payload, uint32_t *payloadBytes) {
    if (!sampleBuffer || !CMSampleBufferDataIsReady(sampleBuffer) ||
        CMSampleBufferGetNumSamples(sampleBuffer) != 1)
        return kVTVideoEncoderMalfunctionErr;

    CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sampleBuffer);
    OSStatus status = update_parameter_sets(worker, format);
    if (status != noErr || !cached_parameter_sets_complete(worker))
        return status != noErr ? status : kVTVideoEncoderMalfunctionErr;

    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sampleBuffer);
    size_t codedBytes = 0;
    status = validate_length_prefixed_access_unit(block, worker->nalLengthBytes, &codedBytes);
    if (status != noErr) return status;
    size_t setBytes = worker->parameterSetBytes[0] + worker->parameterSetBytes[1] +
        worker->parameterSetBytes[2];
    if (setBytes > WEXMaxPacket || codedBytes > WEXMaxPacket - sizeof(*info) - setBytes)
        return memFullErr;
    size_t totalBytes = sizeof(*info) + setBytes + codedBytes;
    if (totalBytes > WEXMaxPacket || totalBytes > UINT32_MAX) return memFullErr;

    uint8_t *result = malloc(totalBytes);
    if (!result) return memFullErr;
    memset(info, 0, sizeof(*info));
    time_to_wire(CMSampleBufferGetPresentationTimeStamp(sampleBuffer),
        &info->ptsValue, &info->ptsScale);
    time_to_wire(CMSampleBufferGetDuration(sampleBuffer),
        &info->durationValue, &info->durationScale);
    info->keyFrame = sample_is_key_frame(sampleBuffer) ? 1u : 0u;
    info->nalLengthBytes = (uint32_t)worker->nalLengthBytes;
    info->vpsBytes = (uint32_t)worker->parameterSetBytes[0];
    info->spsBytes = (uint32_t)worker->parameterSetBytes[1];
    info->ppsBytes = (uint32_t)worker->parameterSetBytes[2];
    info->codedBytes = (uint32_t)codedBytes;

    memcpy(result, info, sizeof(*info));
    size_t offset = sizeof(*info);
    for (unsigned slot = 0; slot < 3; ++slot) {
        memcpy(result + offset, worker->parameterSets[slot], worker->parameterSetBytes[slot]);
        offset += worker->parameterSetBytes[slot];
    }
    status = CMBlockBufferCopyDataBytes(block, 0, codedBytes, result + offset);
    if (status != noErr) {
        free(result);
        return status;
    }
    *payload = result;
    *payloadBytes = (uint32_t)totalBytes;
    return noErr;
}

static OSStatus encode_frame(WEXWorker *worker, const WEXFrameInfo *frameInfo,
    const uint8_t *nv12, uint8_t **response, uint32_t *responseBytes) {
    if (frameInfo->reserved != 0 || frameInfo->color.reserved != 0 ||
        frameInfo->forceKeyFrame > 1) return paramErr;
    size_t width = (size_t)worker->config.width;
    size_t height = (size_t)worker->config.height;
    if (height && width > SIZE_MAX / height) return paramErr;
    size_t yBytes = width * height;
    if (yBytes > SIZE_MAX - yBytes / 2) return paramErr;

    CMTime presentationTime = kCMTimeInvalid;
    CMTime duration = kCMTimeInvalid;
    if (!time_from_wire(frameInfo->ptsValue, frameInfo->ptsScale, &presentationTime) ||
        !time_from_wire(frameInfo->durationValue, frameInfo->durationScale, &duration))
        return paramErr;

    CVPixelBufferRef pixelBuffer = NULL;
    OSStatus status = create_input_pixel_buffer(worker, nv12, &pixelBuffer);
    if (status != noErr) return status;
    status = WEXApplyVideoColorMetadata(pixelBuffer, &frameInfo->color);
    if (status != noErr) {
        CVPixelBufferRelease(pixelBuffer);
        return status;
    }

    uint32_t forceKeyFrame = worker->frameCount == 0 || frameInfo->forceKeyFrame != 0;
    CMSampleBufferRef sampleBuffer = NULL;
    status = WEXNativeHEVCEncode(worker->nativeSession, pixelBuffer,
        presentationTime, duration, forceKeyFrame, &sampleBuffer);
    CVPixelBufferRelease(pixelBuffer);
    if (status != noErr) {
        if (sampleBuffer) CFRelease(sampleBuffer);
        fprintf(stderr, "WEX_WORKER frame=%llu native encode failed status=%d\n",
            (unsigned long long)worker->frameCount, status);
        return status;
    }
    if (!sampleBuffer) return kVTVideoEncoderMalfunctionErr;

    WEXEncodedInfo info = {0};
    uint8_t *samplePayload = NULL;
    uint32_t samplePayloadBytes = 0;
    status = pack_sample(worker, sampleBuffer, &info, &samplePayload, &samplePayloadBytes);
    if (status == noErr && forceKeyFrame && info.keyFrame == 0)
        status = kVTVideoEncoderMalfunctionErr;
    CFRelease(sampleBuffer);
    if (status != noErr) {
        free(samplePayload);
        fprintf(stderr, "WEX_WORKER frame=%llu output validation failed status=%d\n",
            (unsigned long long)worker->frameCount, status);
        return status;
    }

    *response = samplePayload;
    *responseBytes = samplePayloadBytes;
    worker->frameCount++;
    return noErr;
}

static void destroy_worker(WEXWorker *worker) {
    if (worker->nativeSession) {
        WEXNativeHEVEClose(worker->nativeSession);
        worker->nativeSession = NULL;
    }
    for (unsigned slot = 0; slot < 3; ++slot) {
        free(worker->parameterSets[slot]);
        worker->parameterSets[slot] = NULL;
    }
}

static int run_worker(void) {
    WEXWorker worker = {0};
    int opened = 0;
    int exitStatus = 0;
    for (;;) {
        WEXMessage request = {0};
        int cleanEOF = 0;
        if (read_exact(STDIN_FILENO, &request, sizeof(request), &cleanEOF) != 0) {
            fprintf(stderr, "WEX_WORKER truncated or failed message header\n");
            exitStatus = 1;
            break;
        }
        if (cleanEOF) break;
        if (request.magic != WEXWireMagic || request.version != WEXWireVersion ||
            request.status != 0 || request.payloadBytes > WEXMaxPacket) {
            (void)send_reply(request.kind, paramErr, NULL, 0);
            exitStatus = 1;
            break;
        }

        if (request.kind == WEXProbe) {
            if (request.payloadBytes != 0) {
                (void)send_reply(WEXProbe, paramErr, NULL, 0);
                continue;
            }
            WEXProbeInfo info = {0};
            OSStatus status = probe_native_backend(&info, 0);
            int sendStatus = send_reply(WEXProbe, status,
                status == noErr ? &info : NULL,
                status == noErr ? (uint32_t)sizeof(info) : 0);
            if (sendStatus != 0) {
                exitStatus = 1;
                break;
            }
            continue;
        }

        if (request.kind == WEXOpen) {
            if (opened || request.payloadBytes != sizeof(WEXConfig)) {
                (void)send_reply(WEXOpen, paramErr, NULL, 0);
                continue;
            }
            if (read_exact(STDIN_FILENO, &worker.config, sizeof(worker.config), NULL) != 0) {
                (void)send_reply(WEXOpen, paramErr, NULL, 0);
                exitStatus = 1;
                break;
            }
            OSStatus status = open_native_session(&worker);
            if (send_reply(WEXOpen, status, NULL, 0) != 0) {
                exitStatus = 1;
                break;
            }
            if (status != noErr) {
                fprintf(stderr, "WEX_WORKER open failed status=%d\n", status);
                exitStatus = 1;
                break;
            }
            opened = 1;
            continue;
        }

        if (request.kind != WEXFrame || !opened ||
            request.payloadBytes < sizeof(WEXFrameInfo)) {
            (void)send_reply(request.kind, paramErr, NULL, 0);
            if (request.payloadBytes > 0) {
                uint8_t discard[4096];
                uint32_t remaining = request.payloadBytes;
                while (remaining) {
                    size_t amount = remaining < sizeof(discard) ? remaining : sizeof(discard);
                    if (read_exact(STDIN_FILENO, discard, amount, NULL) != 0) {
                        exitStatus = 1;
                        break;
                    }
                    remaining -= (uint32_t)amount;
                }
            }
            if (exitStatus) break;
            continue;
        }

        size_t width = (size_t)worker.config.width;
        size_t height = (size_t)worker.config.height;
        if (height == 0 || width > SIZE_MAX / height) {
            (void)send_reply(WEXFrame, paramErr, NULL, 0);
            exitStatus = 1;
            break;
        }
        size_t yBytes = width * height;
        if (yBytes > SIZE_MAX - yBytes / 2) {
            (void)send_reply(WEXFrame, paramErr, NULL, 0);
            exitStatus = 1;
            break;
        }
        size_t expectedPayloadBytes = sizeof(WEXFrameInfo) + yBytes + yBytes / 2;
        if (request.payloadBytes != expectedPayloadBytes || expectedPayloadBytes > WEXMaxPacket) {
            (void)send_reply(WEXFrame, paramErr, NULL, 0);
            uint32_t remaining = request.payloadBytes;
            uint8_t discard[4096];
            while (remaining) {
                size_t amount = remaining < sizeof(discard) ? remaining : sizeof(discard);
                if (read_exact(STDIN_FILENO, discard, amount, NULL) != 0) {
                    exitStatus = 1;
                    break;
                }
                remaining -= (uint32_t)amount;
            }
            if (exitStatus) break;
            continue;
        }

        uint8_t *frame = malloc(expectedPayloadBytes);
        if (!frame) {
            (void)send_reply(WEXFrame, memFullErr, NULL, 0);
            exitStatus = 1;
            break;
        }
        if (read_exact(STDIN_FILENO, frame, expectedPayloadBytes, NULL) != 0) {
            free(frame);
            (void)send_reply(WEXFrame, paramErr, NULL, 0);
            exitStatus = 1;
            break;
        }

        WEXFrameInfo frameInfo;
        memcpy(&frameInfo, frame, sizeof(frameInfo));
        uint8_t *response = NULL;
        uint32_t responseBytes = 0;
        OSStatus status = encode_frame(&worker, &frameInfo,
            frame + sizeof(frameInfo), &response, &responseBytes);
        free(frame);
        int sendStatus = send_reply(WEXFrame, status, response, responseBytes);
        free(response);
        if (sendStatus != 0) {
            exitStatus = 1;
            break;
        }
    }

    destroy_worker(&worker);
    return exitStatus;
}

int main(void) {
    @autoreleasepool {
        return run_worker();
    }
}
