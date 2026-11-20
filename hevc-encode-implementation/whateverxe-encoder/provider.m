/*
 * Intel HEVC VideoToolbox provider.
 *
 * Registration uses VideoToolbox's private encoder-factory SPI. The encoder
 * object and property handling here are an independent implementation. Frame
 * submission is delegated to the matching WEX transport backend.
 */

#import <CoreFoundation/CoreFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <IOKit/IOKitLib.h>
#import <VideoToolbox/VideoToolbox.h>

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "transport.h"
#include "wire.h"
#include "pixel_input.h"

/* Minimal private CoreMedia base-object ABI declarations. */
typedef struct OpaqueCMBaseObject *CMBaseObjectRef;
typedef struct OpaqueCMBaseClass *CMBaseClassID;
typedef void *VTEncoderSessionRef;
typedef void *VTEncoderFrameRef;
typedef CMBaseObjectRef VTVideoEncoderRef;

#pragma pack(push, 4)
typedef struct {
    uint32_t version;
    size_t derivedStorageSize;
    Boolean (*equal)(CMBaseObjectRef, CMBaseObjectRef);
    OSStatus (*invalidate)(CMBaseObjectRef);
    void (*finalize)(CMBaseObjectRef);
    CFStringRef (*copyDebugDescription)(CMBaseObjectRef);
    OSStatus (*copyProperty)(CMBaseObjectRef, CFStringRef, CFAllocatorRef, void *);
    OSStatus (*setProperty)(CMBaseObjectRef, CFStringRef, CFTypeRef);
    OSStatus (*notificationBarrier)(CMBaseObjectRef);
    const void *protocolTable;
} WEXCMBaseClass;
#pragma pack(pop)

typedef struct {
    const void *reserved;
    const WEXCMBaseClass *baseClass;
} WEXCMBaseVTable;

#pragma pack(push, 4)
typedef struct {
    uint32_t version;
    OSStatus (*startSession)(VTVideoEncoderRef, VTEncoderSessionRef, CMVideoDimensions);
    OSStatus (*encodeFrame)(VTVideoEncoderRef, VTEncoderFrameRef, CVImageBufferRef,
                            CMTime, CMTime, CFDictionaryRef, VTEncodeInfoFlags *);
    OSStatus (*completeFrames)(VTVideoEncoderRef, CMTime);
    OSStatus (*copySupportedPropertyDictionary)(VTVideoEncoderRef, CFDictionaryRef *);
    void *reserved[8];
} WEXVTEncoderClass;
#pragma pack(pop)

typedef struct {
    WEXCMBaseVTable base;
    const WEXVTEncoderClass *encoderClass;
} WEXVTEncoderVTable;

_Static_assert(offsetof(WEXCMBaseClass, derivedStorageSize) == 4,
               "CoreMedia CMBaseClass uses pack(4)");
_Static_assert(offsetof(WEXCMBaseClass, equal) == 12,
               "CoreMedia class callback offsets must match pack(4) ABI");
_Static_assert(offsetof(WEXCMBaseClass, protocolTable) == 68,
               "CoreMedia class callback offsets must match pack(4) ABI");
_Static_assert(sizeof(WEXCMBaseClass) == 76,
               "CoreMedia CMBaseClass size must match 64-bit ABI");
_Static_assert(offsetof(WEXVTEncoderClass, startSession) == 4,
               "VideoToolbox encoder class uses pack(4)");
_Static_assert(offsetof(WEXVTEncoderClass, reserved) == 36,
               "VideoToolbox encoder class callback offsets must match pack(4) ABI");
_Static_assert(sizeof(WEXVTEncoderClass) == 100,
               "VideoToolbox encoder class size must match 64-bit ABI");

extern OSStatus CMDerivedObjectCreate(CFAllocatorRef, const WEXCMBaseVTable *,
                                      CMBaseClassID, CMBaseObjectRef *);
extern void *CMBaseObjectGetDerivedStorage(CMBaseObjectRef);
extern CMBaseClassID VTVideoEncoderGetClassID(void);
extern OSStatus VTRegisterVideoEncoderWithInfo(FourCharCode, CFDictionaryRef, void *);
extern OSStatus VTEncoderSessionSetPixelBufferAttributes(VTEncoderSessionRef, CFDictionaryRef);
extern OSStatus VTEncoderSessionEmitEncodedFrame(VTEncoderSessionRef, VTEncoderFrameRef,
                                                 OSStatus, VTEncodeInfoFlags,
                                                 CMSampleBufferRef);

enum {
    WEXRegistrationRating = 0,
    WEXCMBaseObjectErrorValueNotAvailable = -12783
};

static CFStringRef const WEXEncoderID = CFSTR("org.geekaudio.whateverxe.videoencoder.hevc");
static CFStringRef const WEXEncoderName = CFSTR("WhateverXe Intel HEVC");

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t condition;
    CFMutableDictionaryRef properties;
    VTEncoderSessionRef session;
    WEXEncoderBackend *backend;
    uint64_t gpuRegistryID;
    int32_t width;
    int32_t height;
    int32_t bitrate;
    int32_t fpsNumerator;
    int32_t fpsDenominator;
    int32_t maxKeyFrameInterval;
    uint64_t framesSubmitted;
    unsigned pendingFrames;
    bool backendBusy;
    bool started;
    bool invalidated;
} WEXEncoderState;

typedef struct WEXEmissionScope {
    WEXEncoderState *state;
    struct WEXEmissionScope *previous;
} WEXEmissionScope;

static _Thread_local WEXEmissionScope *gEmissionScope;

static unsigned emissions_on_current_thread(WEXEncoderState *state) {
    unsigned count = 0;
    for (WEXEmissionScope *scope = gEmissionScope; scope; scope = scope->previous) {
        if (scope->state == state) count++;
    }
    return count;
}

static WEXEncoderBackend *detach_backend_if_idle(WEXEncoderState *state) {
    if (!state->invalidated || state->backendBusy || state->pendingFrames != 0) {
        return NULL;
    }
    WEXEncoderBackend *backend = state->backend;
    state->backend = NULL;
    state->session = NULL;
    return backend;
}

static OSStatus emit_frame_and_finish(VTVideoEncoderRef encoder,
                                      WEXEncoderState *state,
                                      VTEncoderSessionRef session,
                                      VTEncoderFrameRef frame,
                                      OSStatus status,
                                      VTEncodeInfoFlags infoFlags,
                                      CMSampleBufferRef sample,
                                      bool releaseBackendBusy) {
    WEXEmissionScope scope = { state, gEmissionScope };
    gEmissionScope = &scope;
    OSStatus emitStatus = VTEncoderSessionEmitEncodedFrame(
        session, frame, status, infoFlags, sample);
    gEmissionScope = scope.previous;

    pthread_mutex_lock(&state->lock);
    if (state->pendingFrames > 0) state->pendingFrames--;
    if (releaseBackendBusy) state->backendBusy = false;
    WEXEncoderBackend *backend = detach_backend_if_idle(state);
    pthread_cond_broadcast(&state->condition);
    pthread_mutex_unlock(&state->lock);

    if (backend) WEXBackendClose(backend);
    CFRelease(encoder);
    return emitStatus;
}

static pthread_once_t gRegistrationOnce = PTHREAD_ONCE_INIT;
static OSStatus gRegistrationStatus = kVTCouldNotFindVideoEncoderErr;
static uint64_t gRegisteredIntelRegistryID;

OSStatus WEXEncoderCreateInstance(FourCharCode codec, CFAllocatorRef allocator,
                                  VTVideoEncoderRef *encoderOut);

static WEXEncoderState *encoder_state(VTVideoEncoderRef encoder) {
    return *(WEXEncoderState **)CMBaseObjectGetDerivedStorage(encoder);
}

static bool read_registry_u16(io_registry_entry_t entry, CFStringRef key, uint16_t *value) {
    CFTypeRef property = IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0);
    if (!property) return false;

    bool valid = false;
    if (CFGetTypeID(property) == CFDataGetTypeID()) {
        CFIndex length = CFDataGetLength((CFDataRef)property);
        if (length >= 2) {
            const UInt8 *bytes = CFDataGetBytePtr((CFDataRef)property);
            *value = (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
            valid = true;
        }
    } else if (CFGetTypeID(property) == CFNumberGetTypeID()) {
        int number = 0;
        if (CFNumberGetValue((CFNumberRef)property, kCFNumberIntType, &number) &&
            number >= 0 && number <= UINT16_MAX) {
            *value = (uint16_t)number;
            valid = true;
        }
    }

    CFRelease(property);
    return valid;
}

static bool accelerator_has_uhd770_parent(io_registry_entry_t accelerator) {
    io_registry_entry_t entry = accelerator;
    IOObjectRetain(entry);
    bool found = false;

    for (unsigned depth = 0; entry && depth < 12; depth++) {
        uint16_t vendor = 0, device = 0;
        bool isIntelUHD770 = read_registry_u16(entry, CFSTR("vendor-id"), &vendor) &&
                             read_registry_u16(entry, CFSTR("device-id"), &device) &&
                             vendor == WEXIntelVendorID && device == WEXUHD770DeviceID;
        if (isIntelUHD770) {
            found = true;
            break;
        }

        io_registry_entry_t parent = IO_OBJECT_NULL;
        if (IORegistryEntryGetParentEntry(entry, kIOServicePlane, &parent) != KERN_SUCCESS)
            break;
        IOObjectRelease(entry);
        entry = parent;
    }

    if (entry) IOObjectRelease(entry);
    return found;
}

static bool supports_hevc_main(CFDictionaryRef capabilities) {
    if (!capabilities || CFGetTypeID(capabilities) != CFDictionaryGetTypeID()) return false;
    CFArrayRef profiles = CFDictionaryGetValue(capabilities, CFSTR("VTSupportedProfileArray"));
    if (!profiles || CFGetTypeID(profiles) != CFArrayGetTypeID()) return false;

    for (CFIndex index = 0; index < CFArrayGetCount(profiles); index++) {
        CFTypeRef value = CFArrayGetValueAtIndex(profiles, index);
        int profile = 0;
        if (value && CFGetTypeID(value) == CFNumberGetTypeID() &&
            CFNumberGetValue((CFNumberRef)value, kCFNumberIntType, &profile) && profile == 1)
            return true;
    }
    return false;
}

static bool find_uhd770_accelerator(uint64_t *registryID) {
    if (!registryID) return false;
    io_iterator_t iterator = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                     IOServiceMatching("IntelAccelerator"),
                                     &iterator) != KERN_SUCCESS)
        return false;

    unsigned matches = 0;
    uint64_t selectedID = 0;
    io_registry_entry_t accelerator;
    while ((accelerator = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        CFTypeRef generation = IORegistryEntryCreateCFProperty(accelerator,
            CFSTR("IOGVACodec"), kCFAllocatorDefault, 0);
        CFTypeRef capabilities = IORegistryEntryCreateCFProperty(accelerator,
            CFSTR("IOGVAHEVCEncodeCapabilities"), kCFAllocatorDefault, 0);
        uint64_t candidateID = 0;
        bool isTigerLakeVA = generation && CFGetTypeID(generation) == CFStringGetTypeID() &&
            CFEqual(generation, CFSTR("Gen10"));
        bool isMainEncoder = supports_hevc_main((CFDictionaryRef)capabilities);
        bool isPhysicalUHD770 = accelerator_has_uhd770_parent(accelerator);
        bool hasRegistryID = IORegistryEntryGetRegistryEntryID(accelerator, &candidateID) == KERN_SUCCESS;

        if (isTigerLakeVA && isMainEncoder && isPhysicalUHD770 && hasRegistryID) {
            selectedID = candidateID;
            matches++;
        }

        if (generation) CFRelease(generation);
        if (capabilities) CFRelease(capabilities);
        IOObjectRelease(accelerator);
    }
    IOObjectRelease(iterator);

    if (matches != 1) return false;
    *registryID = selectedID;
    return true;
}

static CFNumberRef cfnumber_i32(int32_t value) {
    return CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
}

static CFNumberRef cfnumber_u64(uint64_t value) {
    int64_t signedValue = (int64_t)value;
    return CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &signedValue);
}

static CFDictionaryRef property_description(CFStringRef type, CFStringRef access,
                                            CFArrayRef values) {
    const void *keys[3] = { kVTPropertyTypeKey, kVTPropertyReadWriteStatusKey,
                            kVTPropertySupportedValueListKey };
    const void *objects[3] = { type, access, values };
    return CFDictionaryCreate(kCFAllocatorDefault, keys, objects,
        values ? 3 : 2, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
}

static CFDictionaryRef encoder_supported_properties(void) {
    const void *profileValuesRaw[] = { kVTProfileLevel_HEVC_Main_AutoLevel };
    const void *booleanValuesRaw[] = { kCFBooleanFalse, kCFBooleanTrue };
    const void *falseValueRaw[] = { kCFBooleanFalse };
    CFArrayRef profileValues = CFArrayCreate(kCFAllocatorDefault,
        profileValuesRaw, 1, &kCFTypeArrayCallBacks);
    CFArrayRef booleanValues = CFArrayCreate(kCFAllocatorDefault, booleanValuesRaw, 2,
                                              &kCFTypeArrayCallBacks);
    CFArrayRef falseValue = CFArrayCreate(kCFAllocatorDefault, falseValueRaw, 1,
                                           &kCFTypeArrayCallBacks);
    CFDictionaryRef number = property_description(kVTPropertyType_Number,
        kVTPropertyReadWriteStatus_ReadWrite, NULL);
    CFDictionaryRef profile = property_description(kVTPropertyType_Enumeration,
        kVTPropertyReadWriteStatus_ReadWrite, profileValues);
    CFDictionaryRef boolean = property_description(kVTPropertyType_Boolean,
        kVTPropertyReadWriteStatus_ReadWrite, booleanValues);
    CFDictionaryRef noReordering = property_description(kVTPropertyType_Boolean,
        kVTPropertyReadWriteStatus_ReadWrite, falseValue);
    CFDictionaryRef readOnlyBoolean = property_description(kVTPropertyType_Boolean,
        kVTPropertyReadWriteStatus_ReadOnly, NULL);
    CFDictionaryRef readOnlyNumber = property_description(kVTPropertyType_Number,
        kVTPropertyReadWriteStatus_ReadOnly, NULL);

    if (!profileValues || !booleanValues || !falseValue || !number || !profile ||
        !boolean || !noReordering ||
        !readOnlyBoolean || !readOnlyNumber) {
        if (profileValues) CFRelease(profileValues);
        if (booleanValues) CFRelease(booleanValues);
        if (falseValue) CFRelease(falseValue);
        if (number) CFRelease(number);
        if (profile) CFRelease(profile);
        if (boolean) CFRelease(boolean);
        if (noReordering) CFRelease(noReordering);
        if (readOnlyBoolean) CFRelease(readOnlyBoolean);
        if (readOnlyNumber) CFRelease(readOnlyNumber);
        return NULL;
    }

    const void *keys[] = {
        kVTCompressionPropertyKey_AverageBitRate,
        kVTCompressionPropertyKey_ExpectedFrameRate,
        kVTCompressionPropertyKey_ProfileLevel,
        kVTCompressionPropertyKey_MaxKeyFrameInterval,
        kVTCompressionPropertyKey_RealTime,
        kVTCompressionPropertyKey_AllowFrameReordering,
        kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,
        kVTCompressionPropertyKey_UsingGPURegistryID
    };
    const void *values[] = { number, number, profile, number, boolean, noReordering,
                             readOnlyBoolean, readOnlyNumber };
    CFDictionaryRef supported = CFDictionaryCreate(kCFAllocatorDefault, keys, values,
        sizeof(keys) / sizeof(keys[0]), &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);

    CFRelease(profileValues);
    CFRelease(booleanValues);
    CFRelease(falseValue);
    CFRelease(number);
    CFRelease(profile);
    CFRelease(boolean);
    CFRelease(noReordering);
    CFRelease(readOnlyBoolean);
    CFRelease(readOnlyNumber);
    return supported;
}

static CFDictionaryRef encoder_selection_properties(void) {
    const void *profileValue[] = { kVTProfileLevel_HEVC_Main_AutoLevel };
    CFArrayRef profiles = CFArrayCreate(kCFAllocatorDefault, profileValue, 1,
                                         &kCFTypeArrayCallBacks);
    if (!profiles) return NULL;
    const void *keys[] = { kVTCompressionPropertyKey_ProfileLevel };
    const void *values[] = { profiles };
    CFDictionaryRef selection = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(profiles);
    return selection;
}

static void register_intel_hevc_once(void) {
    uint64_t registryID = 0;
    if (!find_uhd770_accelerator(&registryID)) {
        gRegistrationStatus = kVTCouldNotFindVideoEncoderErr;
        return;
    }
    if (!WEXBackendIsAvailable(registryID)) {
        gRegistrationStatus = kVTVideoEncoderNotAvailableNowErr;
        return;
    }

    CFNumberRef rating = cfnumber_i32(WEXRegistrationRating);
    CFDictionaryRef selection = encoder_selection_properties();
    const void *matchingKeys[] = {
        CFSTR("VTCodecType"),
        CFSTR("VTIORegistryCapabilitiesKey"),
        CFSTR("VTIsHardwareAccelerated")
    };
    const void *matchingValues[] = {
        CFSTR("hvc1"),
        CFSTR("IOGVAHEVCEncodeCapabilities"),
        kCFBooleanTrue
    };
    CFDictionaryRef matching = CFDictionaryCreate(kCFAllocatorDefault,
        matchingKeys, matchingValues, 3, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (!rating || !selection || !matching) {
        if (rating) CFRelease(rating);
        if (selection) CFRelease(selection);
        if (matching) CFRelease(matching);
        gRegistrationStatus = memFullErr;
        return;
    }

    const void *keys[] = {
        CFSTR("CMClassImplementationID"),
        CFSTR("CMClassImplementationName"),
        CFSTR("VTRating"),
        CFSTR("VTIsHardwareAccelerated"),
        kVTVideoEncoderList_SupportedSelectionProperties,
        CFSTR("CMMatchingInfo")
    };
    const void *values[] = { WEXEncoderID, WEXEncoderName, rating,
        kCFBooleanTrue, selection, matching };
    CFDictionaryRef info = CFDictionaryCreate(kCFAllocatorDefault, keys, values,
        sizeof(keys) / sizeof(keys[0]), &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (!info) {
        CFRelease(rating);
        CFRelease(selection);
        CFRelease(matching);
        gRegistrationStatus = memFullErr;
        return;
    }

    /* The factory can be called reentrantly during registration, but it never starts the backend. */
    gRegisteredIntelRegistryID = registryID;
    gRegistrationStatus = VTRegisterVideoEncoderWithInfo(kCMVideoCodecType_HEVC,
        info, (void *)WEXEncoderCreateInstance);
    if (gRegistrationStatus != noErr) gRegisteredIntelRegistryID = 0;

    CFRelease(info);
    CFRelease(rating);
    CFRelease(selection);
    CFRelease(matching);
}

OSStatus WEXRegisterIntelHEVCEncoder(void) {
    pthread_once(&gRegistrationOnce, register_intel_hevc_once);
    return gRegistrationStatus;
}

static bool number_to_i32(CFTypeRef value, int32_t *result) {
    if (!value || CFGetTypeID(value) != CFNumberGetTypeID() || !result) return false;
    int64_t number = 0;
    if (!CFNumberGetValue((CFNumberRef)value, kCFNumberSInt64Type, &number) ||
        number < INT32_MIN || number > INT32_MAX)
        return false;
    *result = (int32_t)number;
    return true;
}

static int greatest_common_divisor(int a, int b) {
    while (b != 0) {
        int remainder = a % b;
        a = b;
        b = remainder;
    }
    return a > 0 ? a : 1;
}

static bool frame_rate_from_value(CFTypeRef value, int32_t *numerator,
                                  int32_t *denominator) {
    if (!value || CFGetTypeID(value) != CFNumberGetTypeID()) return false;
    double framesPerSecond = 0.0;
    if (!CFNumberGetValue((CFNumberRef)value, kCFNumberDoubleType, &framesPerSecond) ||
        framesPerSecond <= 0.0 || framesPerSecond > 240.0)
        return false;

    int rawNumerator = (int)(framesPerSecond * 1001.0 + 0.5);
    int rawDenominator = 1001;
    int divisor = greatest_common_divisor(rawNumerator, rawDenominator);
    *numerator = rawNumerator / divisor;
    *denominator = rawDenominator / divisor;
    return true;
}

static bool frame_rate_from_duration(CMTime duration, int32_t *numerator,
                                     int32_t *denominator) {
    if (!CMTIME_IS_NUMERIC(duration) || duration.value <= 0 ||
        duration.timescale <= 0 || duration.value > INT32_MAX ||
        duration.timescale > INT32_MAX)
        return false;

    int rawNumerator = duration.timescale;
    int rawDenominator = (int)duration.value;
    int divisor = greatest_common_divisor(rawNumerator, rawDenominator);
    *numerator = rawNumerator / divisor;
    *denominator = rawDenominator / divisor;
    return *numerator > 0 && *denominator > 0;
}

static bool is_boolean(CFTypeRef value) {
    return value && CFGetTypeID(value) == CFBooleanGetTypeID();
}

static bool pixel_aspect_is_square(CFTypeRef value) {
    if (!value) return true;
    if (CFGetTypeID(value) != CFDictionaryGetTypeID() ||
        CFDictionaryGetCount((CFDictionaryRef)value) != 2)
        return false;

    CFTypeRef horizontal = CFDictionaryGetValue((CFDictionaryRef)value,
        kCMFormatDescriptionKey_PixelAspectRatioHorizontalSpacing);
    CFTypeRef vertical = CFDictionaryGetValue((CFDictionaryRef)value,
        kCMFormatDescriptionKey_PixelAspectRatioVerticalSpacing);
    int64_t h = 0, v = 0;
    return horizontal && vertical &&
        CFGetTypeID(horizontal) == CFNumberGetTypeID() &&
        CFGetTypeID(vertical) == CFNumberGetTypeID() &&
        CFNumberGetValue((CFNumberRef)horizontal, kCFNumberSInt64Type, &h) &&
        CFNumberGetValue((CFNumberRef)vertical, kCFNumberSInt64Type, &v) &&
        h > 0 && v > 0 && h == v;
}

static OSStatus encoder_set_property(CMBaseObjectRef object, CFStringRef key,
                                     CFTypeRef value) {
    WEXEncoderState *state = encoder_state(object);
    if (!state || !key) return kVTParameterErr;

    if (CFEqual(key, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder) ||
        CFEqual(key, kVTCompressionPropertyKey_UsingGPURegistryID))
        return kVTPropertyNotSupportedErr;

    bool recognized = CFEqual(key, kVTCompressionPropertyKey_AverageBitRate) ||
        CFEqual(key, kVTCompressionPropertyKey_ExpectedFrameRate) ||
        CFEqual(key, kVTCompressionPropertyKey_ProfileLevel) ||
        CFEqual(key, kVTCompressionPropertyKey_PixelAspectRatio) ||
        CFEqual(key, kVTCompressionPropertyKey_MaxKeyFrameInterval) ||
        CFEqual(key, kVTCompressionPropertyKey_RealTime) ||
        CFEqual(key, kVTCompressionPropertyKey_AllowFrameReordering);
    if (!recognized) return kVTPropertyNotSupportedErr;
    bool pixelAspectRatio =
        CFEqual(key, kVTCompressionPropertyKey_PixelAspectRatio);
    if (pixelAspectRatio) {
        if (!pixel_aspect_is_square(value)) return kVTPropertyNotSupportedErr;
    } else if (!value) {
        return kVTParameterErr;
    }

    if (pixelAspectRatio) {
        /* Square pixels are the provider's native bitstream format. */
    } else if (CFEqual(key, kVTCompressionPropertyKey_AverageBitRate)) {
        int32_t bitrate = 0;
        if (!number_to_i32(value, &bitrate) || bitrate < 100000 ||
            bitrate > 100000000)
            return kVTParameterErr;
    } else if (CFEqual(key, kVTCompressionPropertyKey_ExpectedFrameRate)) {
        int32_t numerator = 0, denominator = 0;
        if (!frame_rate_from_value(value, &numerator, &denominator)) return kVTParameterErr;
    } else if (CFEqual(key, kVTCompressionPropertyKey_ProfileLevel)) {
        if (CFGetTypeID(value) != CFStringGetTypeID() ||
            !CFEqual(value, kVTProfileLevel_HEVC_Main_AutoLevel))
            return kVTPropertyNotSupportedErr;
    } else if (CFEqual(key, kVTCompressionPropertyKey_MaxKeyFrameInterval)) {
        int32_t interval = 0;
        if (!number_to_i32(value, &interval) || interval < 0 || interval > 1000000)
            return kVTParameterErr;
    } else if (CFEqual(key, kVTCompressionPropertyKey_AllowFrameReordering)) {
        if (!is_boolean(value)) return kVTParameterErr;
        if (CFBooleanGetValue(value)) return kVTPropertyNotSupportedErr;
    } else if (!is_boolean(value)) {
        return kVTParameterErr;
    }

    pthread_mutex_lock(&state->lock);
    if (state->invalidated) {
        pthread_mutex_unlock(&state->lock);
        return kVTInvalidSessionErr;
    }

    if (state->backend || state->backendBusy) {
        CFTypeRef current = CFDictionaryGetValue(state->properties, key);
        bool same = pixelAspectRatio
            ? pixel_aspect_is_square(current) && pixel_aspect_is_square(value)
            : current && value && CFEqual(current, value);
        pthread_mutex_unlock(&state->lock);
        return same ? noErr : kVTPropertyNotSupportedErr;
    }

    if (pixelAspectRatio && !value)
        CFDictionaryRemoveValue(state->properties, key);
    else if (value)
        CFDictionarySetValue(state->properties, key, value);
    if (CFEqual(key, kVTCompressionPropertyKey_MaxKeyFrameInterval)) {
        int32_t interval = 0;
        (void)number_to_i32(value, &interval);
        state->maxKeyFrameInterval = interval;
    }
    pthread_mutex_unlock(&state->lock);
    return noErr;
}

static OSStatus encoder_copy_property(CMBaseObjectRef object, CFStringRef key,
                                      CFAllocatorRef allocator, void *output) {
    (void)allocator;
    WEXEncoderState *state = encoder_state(object);
    if (!state || !key || !output) return kVTParameterErr;

    if (CFEqual(key, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder)) {
        *(CFTypeRef *)output = CFRetain(kCFBooleanTrue);
        return noErr;
    }
    if (CFEqual(key, kVTCompressionPropertyKey_UsingGPURegistryID)) {
        CFNumberRef value = cfnumber_u64(state->gpuRegistryID);
        if (!value) return memFullErr;
        *(CFTypeRef *)output = value;
        return noErr;
    }

    pthread_mutex_lock(&state->lock);
    CFTypeRef value = CFDictionaryGetValue(state->properties, key);
    if (value) CFRetain(value);
    pthread_mutex_unlock(&state->lock);
    if (!value) return WEXCMBaseObjectErrorValueNotAvailable;
    *(CFTypeRef *)output = value;
    return noErr;
}

static Boolean encoder_equal(CMBaseObjectRef left, CMBaseObjectRef right) {
    return left == right;
}

static CFStringRef encoder_debug_description(CMBaseObjectRef object) {
    WEXEncoderState *state = encoder_state(object);
    if (!state) return CFStringCreateCopy(kCFAllocatorDefault, CFSTR("Intel HEVC encoder"));
    pthread_mutex_lock(&state->lock);
    int32_t width = state->width;
    int32_t height = state->height;
    uint64_t gpu = state->gpuRegistryID;
    pthread_mutex_unlock(&state->lock);
    return CFStringCreateWithFormat(kCFAllocatorDefault, NULL,
        CFSTR("Intel HEVC encoder %d x %d on GPU 0x%llx"), width, height,
        (unsigned long long)gpu);
}

static OSStatus encoder_start_session(VTVideoEncoderRef encoder,
                                      VTEncoderSessionRef session,
                                      CMVideoDimensions dimensions) {
    WEXEncoderState *state = encoder_state(encoder);
    if (!state || !session || dimensions.width <= 0 || dimensions.height <= 0 ||
        dimensions.width > WEXMaxWidth || dimensions.height > WEXMaxHeight ||
        (dimensions.width & 1) || (dimensions.height & 1))
        return kVTCouldNotFindVideoEncoderErr;

    pthread_mutex_lock(&state->lock);
    if (state->invalidated || state->started) {
        pthread_mutex_unlock(&state->lock);
        return kVTInvalidSessionErr;
    }
    state->session = session;
    state->width = dimensions.width;
    state->height = dimensions.height;
    state->started = true;
    pthread_mutex_unlock(&state->lock);

    int32_t width = dimensions.width;
    int32_t height = dimensions.height;
    CFNumberRef nv12Number = cfnumber_i32(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
    CFNumberRef bgraNumber = cfnumber_i32(kCVPixelFormatType_32BGRA);
    CFNumberRef widthNumber = cfnumber_i32(width);
    CFNumberRef heightNumber = cfnumber_i32(height);
    const void *pixelFormats[] = { nv12Number, bgraNumber };
    CFArrayRef pixelFormatValues = nv12Number && bgraNumber
        ? CFArrayCreate(kCFAllocatorDefault, pixelFormats, 2, &kCFTypeArrayCallBacks)
        : NULL;
    CFDictionaryRef ioSurface = CFDictionaryCreate(kCFAllocatorDefault, NULL, NULL,
        0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!nv12Number || !bgraNumber || !pixelFormatValues ||
        !widthNumber || !heightNumber || !ioSurface) {
        if (nv12Number) CFRelease(nv12Number);
        if (bgraNumber) CFRelease(bgraNumber);
        if (pixelFormatValues) CFRelease(pixelFormatValues);
        if (widthNumber) CFRelease(widthNumber);
        if (heightNumber) CFRelease(heightNumber);
        if (ioSurface) CFRelease(ioSurface);
        pthread_mutex_lock(&state->lock);
        if (state->session == session) {
            state->session = NULL;
            state->started = false;
        }
        pthread_mutex_unlock(&state->lock);
        return memFullErr;
    }

    const void *keys[] = { kCVPixelBufferPixelFormatTypeKey,
                           kCVPixelBufferWidthKey,
                           kCVPixelBufferHeightKey,
                           kCVPixelBufferIOSurfacePropertiesKey };
    const void *values[] = { pixelFormatValues, widthNumber, heightNumber, ioSurface };
    CFDictionaryRef attributes = CFDictionaryCreate(kCFAllocatorDefault, keys, values,
        4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(nv12Number);
    CFRelease(bgraNumber);
    CFRelease(pixelFormatValues);
    CFRelease(widthNumber);
    CFRelease(heightNumber);
    CFRelease(ioSurface);
    OSStatus status = attributes
        ? VTEncoderSessionSetPixelBufferAttributes(session, attributes)
        : memFullErr;
    if (attributes) CFRelease(attributes);
    if (status != noErr) {
        pthread_mutex_lock(&state->lock);
        if (state->session == session) {
            state->session = NULL;
            state->started = false;
        }
        pthread_mutex_unlock(&state->lock);
    }
    return status;
}

static OSStatus backend_configuration(WEXEncoderState *state, CMTime duration,
                                      int32_t *bitrateOut, int32_t *fpsNumeratorOut,
                                      int32_t *fpsDenominatorOut) {
    int32_t bitrate = 0;
    CFTypeRef bitrateValue = CFDictionaryGetValue(state->properties,
        kVTCompressionPropertyKey_AverageBitRate);
    if (bitrateValue && !number_to_i32(bitrateValue, &bitrate)) return kVTParameterErr;

    int32_t fpsNumerator = 0, fpsDenominator = 0;
    CFTypeRef fpsValue = CFDictionaryGetValue(state->properties,
        kVTCompressionPropertyKey_ExpectedFrameRate);
    bool validRate = fpsValue && frame_rate_from_value(fpsValue,
        &fpsNumerator, &fpsDenominator);
    if (!validRate) validRate = frame_rate_from_duration(duration,
        &fpsNumerator, &fpsDenominator);
    if (!validRate) {
        fpsNumerator = 30;
        fpsDenominator = 1;
    }

    if (bitrate <= 0) {
        double fps = (double)fpsNumerator / (double)fpsDenominator;
        double suggested = (double)state->width * (double)state->height * fps * 0.10;
        if (suggested < 100000.0) suggested = 100000.0;
        if (suggested > 100000000.0) suggested = 100000000.0;
        bitrate = (int32_t)(suggested + 0.5);
    }

    *bitrateOut = bitrate;
    *fpsNumeratorOut = fpsNumerator;
    *fpsDenominatorOut = fpsDenominator;
    return noErr;
}

static OSStatus encoder_encode_frame(VTVideoEncoderRef encoder,
                                     VTEncoderFrameRef frame,
                                     CVImageBufferRef image,
                                     CMTime presentationTime,
                                     CMTime duration,
                                     CFDictionaryRef frameOptions,
                                     VTEncodeInfoFlags *infoFlags) {
    if (infoFlags) *infoFlags = 0;
    if (!encoder || !frame || !image) return kVTParameterErr;

    /* Keep the derived state alive if the output callback invalidates the session. */
    CFRetain(encoder);
    WEXEncoderState *state = encoder_state(encoder);
    if (!state) {
        CFRelease(encoder);
        return kVTParameterErr;
    }

    pthread_mutex_lock(&state->lock);
    if (state->invalidated || !state->started || !state->session) {
        pthread_mutex_unlock(&state->lock);
        CFRelease(encoder);
        return kVTInvalidSessionErr;
    }

    VTEncoderSessionRef session = state->session;
    state->pendingFrames++;
    OSType sourceFormat = CVPixelBufferGetPixelFormatType((CVPixelBufferRef)image);
    bool pixelBufferSupported =
        (sourceFormat == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
         sourceFormat == kCVPixelFormatType_32BGRA) &&
        CVPixelBufferGetWidth((CVPixelBufferRef)image) == (size_t)state->width &&
        CVPixelBufferGetHeight((CVPixelBufferRef)image) == (size_t)state->height;
    if (!pixelBufferSupported) {
        pthread_mutex_unlock(&state->lock);
        return emit_frame_and_finish(encoder, state, session, frame,
            kVTPixelTransferNotSupportedErr, 0, NULL, false);
    }

    while (!state->invalidated && state->backendBusy) {
        if (emissions_on_current_thread(state) != 0) {
            pthread_mutex_unlock(&state->lock);
            return emit_frame_and_finish(encoder, state, session, frame,
                kVTVideoEncoderNotAvailableNowErr, 0, NULL, false);
        }
        pthread_cond_wait(&state->condition, &state->lock);
    }
    if (state->invalidated) {
        pthread_mutex_unlock(&state->lock);
        return emit_frame_and_finish(encoder, state, session, frame,
            kVTInvalidSessionErr, 0, NULL, false);
    }

    state->backendBusy = true;
    WEXEncoderBackend *backend = state->backend;
    bool openedHere = false;
    int32_t bitrate = 0, fpsNumerator = 0, fpsDenominator = 0;
    OSStatus status = backend ? noErr : backend_configuration(state, duration,
        &bitrate, &fpsNumerator, &fpsDenominator);
    int32_t width = state->width;
    int32_t height = state->height;
    uint64_t frameNumber = state->framesSubmitted;
    int32_t keyFrameInterval = state->maxKeyFrameInterval;
    bool forceKeyFrame = false;
    if (frameOptions) {
        CFTypeRef forceKey = CFDictionaryGetValue(frameOptions,
            kVTEncodeFrameOptionKey_ForceKeyFrame);
        forceKeyFrame = is_boolean(forceKey) && CFBooleanGetValue(forceKey);
    }
    forceKeyFrame = forceKeyFrame || frameNumber == 0 ||
        (keyFrameInterval > 0 && frameNumber % (uint64_t)keyFrameInterval == 0);
    pthread_mutex_unlock(&state->lock);

    if (status == noErr && !backend) {
        status = WEXBackendOpen(state->gpuRegistryID, width, height, bitrate, fpsNumerator,
                                fpsDenominator, &backend);
        if (status == noErr && !backend) status = kVTVideoEncoderMalfunctionErr;
        if (status == noErr) openedHere = true;
        else if (backend) {
            WEXBackendClose(backend);
            backend = NULL;
        }
    }

    CVPixelBufferRef nv12 = NULL;
    if (status == noErr)
        status = WEXCopyVideoRangeNV12((CVPixelBufferRef)image, &nv12);
    if (status == noErr && !nv12) status = kVTVideoEncoderMalfunctionErr;
    if (status == noErr &&
        (CVPixelBufferGetPixelFormatType(nv12) !=
             kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
         CVPixelBufferGetWidth(nv12) != (size_t)width ||
         CVPixelBufferGetHeight(nv12) != (size_t)height))
        status = kVTPixelTransferNotSupportedErr;

    CMSampleBufferRef encoded = NULL;
    if (status == noErr)
        status = WEXBackendFrame(backend, nv12, presentationTime, duration,
                                 forceKeyFrame, &encoded);
    if (nv12) CVPixelBufferRelease(nv12);
    if (status == noErr && !encoded) status = kVTVideoEncoderMalfunctionErr;
    if (status == noErr && !CMSampleBufferDataIsReady(encoded))
        status = kVTVideoEncoderMalfunctionErr;

    pthread_mutex_lock(&state->lock);
    if (openedHere) {
        state->backend = backend;
        state->bitrate = bitrate;
        state->fpsNumerator = fpsNumerator;
        state->fpsDenominator = fpsDenominator;
    }
    if (status == noErr) state->framesSubmitted++;
    /* Keep the operation active until the completion callback returns. */
    pthread_cond_broadcast(&state->condition);
    pthread_mutex_unlock(&state->lock);

    OSStatus emitStatus = emit_frame_and_finish(encoder, state, session, frame,
        status, infoFlags ? *infoFlags : 0, status == noErr ? encoded : NULL, true);
    if (encoded) CFRelease(encoded);

    if (status != noErr) return status;
    return emitStatus;
}

static OSStatus encoder_complete_frames(VTVideoEncoderRef encoder, CMTime untilTime) {
    (void)untilTime;
    WEXEncoderState *state = encoder_state(encoder);
    if (!state) return kVTParameterErr;
    pthread_mutex_lock(&state->lock);
    unsigned currentEmissions = emissions_on_current_thread(state);
    while (!state->invalidated && state->pendingFrames > currentEmissions)
    {
        if (currentEmissions != 0) {
            pthread_mutex_unlock(&state->lock);
            return kVTVideoEncoderNotAvailableNowErr;
        }
        pthread_cond_wait(&state->condition, &state->lock);
    }
    bool valid = !state->invalidated;
    pthread_mutex_unlock(&state->lock);
    return valid ? noErr : kVTInvalidSessionErr;
}

static OSStatus encoder_copy_supported_properties(VTVideoEncoderRef encoder,
                                                  CFDictionaryRef *propertiesOut) {
    (void)encoder;
    if (!propertiesOut) return kVTParameterErr;
    *propertiesOut = encoder_supported_properties();
    return *propertiesOut ? noErr : memFullErr;
}

static OSStatus encoder_invalidate(CMBaseObjectRef object) {
    WEXEncoderState *state = encoder_state(object);
    if (!state) return noErr;

    pthread_mutex_lock(&state->lock);
    state->invalidated = true;
    pthread_cond_broadcast(&state->condition);
    unsigned currentEmissions = emissions_on_current_thread(state);
    if (currentEmissions != 0) {
        pthread_mutex_unlock(&state->lock);
        return noErr;
    }
    while (state->backendBusy || state->pendingFrames > 0)
        pthread_cond_wait(&state->condition, &state->lock);

    WEXEncoderBackend *backend = detach_backend_if_idle(state);
    pthread_mutex_unlock(&state->lock);

    if (backend) WEXBackendClose(backend);
    return noErr;
}

static void encoder_finalize(CMBaseObjectRef object) {
    WEXEncoderState *state = encoder_state(object);
    if (!state) return;
    encoder_invalidate(object);
    if (state->properties) CFRelease(state->properties);
    pthread_cond_destroy(&state->condition);
    pthread_mutex_destroy(&state->lock);
    free(state);
    *(WEXEncoderState **)CMBaseObjectGetDerivedStorage(object) = NULL;
}

static const struct {
    uint32_t alignment;
    WEXCMBaseClass value;
} __attribute__((aligned(8))) gBaseClass = {
    0,
    {
        .version = 1,
        .derivedStorageSize = sizeof(WEXEncoderState *),
        .equal = encoder_equal,
        .invalidate = encoder_invalidate,
        .finalize = encoder_finalize,
        .copyDebugDescription = encoder_debug_description,
        .copyProperty = encoder_copy_property,
        .setProperty = encoder_set_property
    }
};

static const struct {
    uint32_t alignment;
    WEXVTEncoderClass value;
} __attribute__((aligned(8))) gEncoderClass = {
    0,
    {
        .version = 1,
        .startSession = encoder_start_session,
        .encodeFrame = encoder_encode_frame,
        .completeFrames = encoder_complete_frames,
        .copySupportedPropertyDictionary = encoder_copy_supported_properties
    }
};

static const WEXVTEncoderVTable gEncoderVTable = {
    .base = { NULL, &gBaseClass.value },
    .encoderClass = &gEncoderClass.value
};

_Static_assert(offsetof(__typeof__(gBaseClass), value) == 4,
               "CoreMedia base-class pointer must address the packed class");
_Static_assert(offsetof(__typeof__(gEncoderClass), value) == 4,
               "VideoToolbox encoder-class pointer must address the packed class");
_Static_assert(offsetof(WEXCMBaseVTable, baseClass) == 8,
               "CoreMedia vtable uses native pointer alignment");
_Static_assert(offsetof(WEXVTEncoderVTable, encoderClass) == 16,
               "VideoToolbox vtable uses native pointer alignment");

OSStatus WEXEncoderCreateInstance(FourCharCode codec, CFAllocatorRef allocator,
                                  VTVideoEncoderRef *encoderOut) {
    if (codec != kCMVideoCodecType_HEVC || !encoderOut) return kVTCouldNotFindVideoEncoderErr;
    *encoderOut = NULL;
    if (gRegisteredIntelRegistryID == 0) return kVTCouldNotFindVideoEncoderErr;

    CMBaseObjectRef object = NULL;
    OSStatus status = CMDerivedObjectCreate(allocator, &gEncoderVTable.base,
        VTVideoEncoderGetClassID(), &object);
    if (status != noErr) return status;

    WEXEncoderState *state = calloc(1, sizeof(*state));
    if (!state) {
        CFRelease(object);
        return memFullErr;
    }
    if (pthread_mutex_init(&state->lock, NULL) != 0) {
        free(state);
        CFRelease(object);
        return memFullErr;
    }
    if (pthread_cond_init(&state->condition, NULL) != 0) {
        pthread_mutex_destroy(&state->lock);
        free(state);
        CFRelease(object);
        return memFullErr;
    }
    state->properties = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!state->properties) {
        pthread_cond_destroy(&state->condition);
        pthread_mutex_destroy(&state->lock);
        free(state);
        CFRelease(object);
        return memFullErr;
    }
    CFDictionarySetValue(state->properties, kVTCompressionPropertyKey_ProfileLevel,
                         kVTProfileLevel_HEVC_Main_AutoLevel);
    CFDictionarySetValue(state->properties, kVTCompressionPropertyKey_RealTime,
                         kCFBooleanFalse);
    CFDictionarySetValue(state->properties, kVTCompressionPropertyKey_AllowFrameReordering,
                         kCFBooleanFalse);
    state->gpuRegistryID = gRegisteredIntelRegistryID;

    *(WEXEncoderState **)CMBaseObjectGetDerivedStorage(object) = state;
    *encoderOut = object;
    return noErr;
}
