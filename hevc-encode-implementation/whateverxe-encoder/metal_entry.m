#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <Security/SecCode.h>
#import <Security/SecStaticCode.h>
#import <objc/message.h>
#import <objc/runtime.h>
#import <os/log.h>

#include <stdatomic.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <mach/mach.h>

#import <VideoToolbox/VideoToolbox.h>

/* This entry is deliberately tied to the single package identity selected by
 * WhateverXe for Intel UHD 770 (8086:4680). The normal Metal implementation
 * remains Apple's exact installed TGL driver; this bundle adds only an
 * Objective-C subclass with the same initializer ABI. */
static NSString * const WEXNativeBundlePath =
    @"/Library/GPUBundles/AppleIntelTGLGraphicsMTLDriver.bundle";
static NSString * const WEXNativeBundleIdentifier =
    @"com.apple.driver.AppleIntelTGLGraphicsMTLDriver";
static NSString * const WEXNativeExecutable =
    @"AppleIntelTGLGraphicsMTLDriver";
static NSString * const WEXNativePrincipalName = @"MTLIGAccelDevice";
static NSString * const WEXOwnBundleIdentifier =
    @"org.whateverxe.driver.WEXHEVCMetalDriver";
static NSString * const WEXOwnExecutable = @"WEXHEVCMetalDriver";
static NSString * const WEXOwnPrincipalName = @"WEXIntelTGLMetalDevice";
static NSString * const WEXSelectedPluginName = @"WEXHEVCMetalDriver";
static const uint32_t WEXTargetVendorID = 0x8086;
static const uint32_t WEXTargetDeviceID = 0x4680;

extern OSStatus WEXRegisterIntelHEVCEncoder(void);

static Class WEXNativeDeviceClass;
static Class WEXPrincipalDeviceClass;
static dispatch_once_t WEXPrincipalSetupOnce;
static NSError *WEXPrincipalSetupError;
static atomic_int WEXRegistrationState = ATOMIC_VAR_INIT(0);

static BOOL WEXHasExactSelectedIntelAccelerator(void);

static NSError *WEXEntryError(NSInteger code, NSString *detail) {
    return [NSError errorWithDomain:WEXOwnBundleIdentifier
                               code:code
                           userInfo:@{NSLocalizedDescriptionKey: detail ?: @"Metal entry initialization failed"}];
}

static NSString *WEXHexString(const uint8_t *bytes, size_t length) {
    static const char digits[] = "0123456789abcdef";
    if (!bytes || !length || length > SIZE_MAX / 2) return nil;
    NSMutableData *buffer = [NSMutableData dataWithLength:length * 2];
    char *result = buffer.mutableBytes;
    for (size_t i = 0; i < length; ++i) {
        result[2 * i] = digits[bytes[i] >> 4];
        result[2 * i + 1] = digits[bytes[i] & 0x0f];
    }
    return [[NSString alloc] initWithBytes:result
                                    length:length * 2
                                  encoding:NSASCIIStringEncoding];
}

static NSString *WEXBundlePathForImage(void) {
    Dl_info image = {0};
    if (!dladdr((const void *)&WEXBundlePathForImage, &image) ||
        !image.dli_fname || !image.dli_fname[0]) return nil;

    char resolved[PATH_MAX];
    if (!realpath(image.dli_fname, resolved)) return nil;

    NSString *imagePath = [NSString stringWithUTF8String:resolved];
    NSString *macOSPath = [imagePath stringByDeletingLastPathComponent];
    if (![[macOSPath lastPathComponent] isEqualToString:@"MacOS"] ||
        ![[macOSPath.stringByDeletingLastPathComponent lastPathComponent]
             isEqualToString:@"Contents"]) return nil;
    NSString *bundlePath = macOSPath.stringByDeletingLastPathComponent.stringByDeletingLastPathComponent;
    return [[NSFileManager defaultManager] fileExistsAtPath:bundlePath] ? bundlePath : nil;
}

static BOOL WEXCodeHashForBundle(NSString *path, NSString **hashOut, NSError **errorOut) {
    NSURL *url = [NSURL fileURLWithPath:path isDirectory:YES];
    SecStaticCodeRef code = NULL;
    OSStatus status = SecStaticCodeCreateWithPath((__bridge CFURLRef)url,
                                                   kSecCSDefaultFlags, &code);
    if (status != errSecSuccess || !code) {
        if (errorOut) *errorOut = WEXEntryError(status, @"Could not open the installed native Metal driver for signature verification");
        if (code) CFRelease(code);
        return NO;
    }

    status = SecStaticCodeCheckValidity(code, kSecCSStrictValidate, NULL);
    CFDictionaryRef information = NULL;
    if (status == errSecSuccess) {
        status = SecCodeCopySigningInformation((SecCodeRef)code,
                                               kSecCSSigningInformation,
                                               &information);
    }
    CFDataRef unique = information ? CFDictionaryGetValue(information,
                                                           kSecCodeInfoUnique) : NULL;
    if (status != errSecSuccess || !unique || CFGetTypeID(unique) != CFDataGetTypeID()) {
        if (errorOut) *errorOut = WEXEntryError(status, @"The installed native Metal driver has no valid strict code signature");
        if (information) CFRelease(information);
        CFRelease(code);
        return NO;
    }

    NSString *hash = WEXHexString(CFDataGetBytePtr(unique),
                                  (size_t)CFDataGetLength(unique));
    if (information) CFRelease(information);
    CFRelease(code);
    if (!hash) {
        if (errorOut) *errorOut = WEXEntryError(errSecInternalComponent, @"Could not read the native Metal driver code hash");
        return NO;
    }
    if (hashOut) *hashOut = hash;
    return YES;
}

static BOOL WEXNativeBundleMetadataIsExact(NSBundle **bundleOut,
                                           NSString **expectedHashOut,
                                           NSError **errorOut) {
    NSString *resolvedNativePath = [WEXNativeBundlePath stringByResolvingSymlinksInPath];
    if (![resolvedNativePath isEqualToString:WEXNativeBundlePath]) {
        if (errorOut) *errorOut = WEXEntryError(1, @"The native Metal driver path resolved outside the fixed installation path");
        return NO;
    }

    NSBundle *nativeBundle = [NSBundle bundleWithPath:WEXNativeBundlePath];
    NSDictionary *info = nativeBundle.infoDictionary;
    if (!nativeBundle ||
        ![info[@"CFBundleIdentifier"] isEqual:WEXNativeBundleIdentifier] ||
        ![info[@"CFBundleExecutable"] isEqual:WEXNativeExecutable] ||
        ![info[@"NSPrincipalClass"] isEqual:WEXNativePrincipalName]) {
        if (errorOut) *errorOut = WEXEntryError(2, @"The installed native bundle identity or principal class is not the expected TGL Metal driver");
        return NO;
    }

    NSString *ourPath = WEXBundlePathForImage();
    NSBundle *ourBundle = ourPath ? [NSBundle bundleWithPath:ourPath] : nil;
    if (!ourBundle ||
        ![ourBundle.bundleIdentifier isEqual:WEXOwnBundleIdentifier] ||
        ![ourBundle.infoDictionary[@"CFBundleExecutable"] isEqual:WEXOwnExecutable]) {
        if (errorOut) *errorOut = WEXEntryError(3, @"This Mach-O image is not inside the fixed WEX Metal driver bundle");
        return NO;
    }
    NSString *expectedHash = ourBundle.infoDictionary[@"WEXNativeMetalDriverCDHash"];
    if (![expectedHash isKindOfClass:[NSString class]] || expectedHash.length < 32) {
        if (errorOut) *errorOut = WEXEntryError(4, @"The package has no pinned native TGL driver code hash");
        return NO;
    }

    if (bundleOut) *bundleOut = nativeBundle;
    if (expectedHashOut) *expectedHashOut = expectedHash;
    return YES;
}

static BOOL WEXNativeBundleIsExact(NSBundle **bundleOut, NSError **errorOut) {
    NSBundle *nativeBundle = nil;
    NSString *expectedHash = nil;
    if (!WEXNativeBundleMetadataIsExact(&nativeBundle, &expectedHash, errorOut))
        return NO;

    NSString *actualHash = nil;
    NSError *signatureError = nil;
    if (!WEXCodeHashForBundle(WEXNativeBundlePath, &actualHash, &signatureError) ||
        [actualHash caseInsensitiveCompare:expectedHash] != NSOrderedSame) {
        if (errorOut) *errorOut = signatureError ?: WEXEntryError(5, @"The installed native TGL driver code hash differs from the package pin");
        return NO;
    }
    if (bundleOut) *bundleOut = nativeBundle;
    return YES;
}

static BOOL WEXMethodHasAcceleratorPortABI(Method method) {
    if (!method || method_getNumberOfArguments(method) != 3) return NO;
    char returnType[32] = {0};
    char selfType[32] = {0};
    char selectorType[32] = {0};
    char argumentType[32] = {0};
    method_getReturnType(method, returnType, sizeof(returnType));
    method_getArgumentType(method, 0, selfType, sizeof(selfType));
    method_getArgumentType(method, 1, selectorType, sizeof(selectorType));
    method_getArgumentType(method, 2, argumentType, sizeof(argumentType));
    const char *encoding = method_getTypeEncoding(method);
    return encoding && strcmp(encoding, "@20@0:8I16") == 0 &&
           returnType[0] == '@' && selfType[0] == '@' &&
           selectorType[0] == ':' && argumentType[0] == 'I';
}

static id WEXInitWithAcceleratorPort(id self, SEL command,
                                     mach_port_t acceleratorPort) {
    NSError *signatureError = nil;
    if (!WEXNativeBundleIsExact(NULL, &signatureError)) {
        os_log_error(OS_LOG_DEFAULT,
            "WEXHEVCMetalDriver: native TGL signature check failed (%{public}@)",
            signatureError.localizedDescription);
        return nil;
    }

    struct objc_super superInfo = { self, WEXNativeDeviceClass };
    id initialized = ((id (*)(struct objc_super *, SEL, mach_port_t))objc_msgSendSuper)(
        &superInfo, command, acceleratorPort);
    if (initialized) {
        /* Run after Apple's initializer has returned, when the selected
         * accelerator is already registered. This avoids querying or
         * mutating IORegistry during service/class initialization. */
        int expected = 0;
        if (atomic_compare_exchange_strong_explicit(&WEXRegistrationState,
                &expected, 1, memory_order_acq_rel, memory_order_acquire)) {
            if (WEXHasExactSelectedIntelAccelerator()) {
                OSStatus status = WEXRegisterIntelHEVCEncoder();
                if (status != noErr) {
                    os_log_error(OS_LOG_DEFAULT,
                        "WEXHEVCMetalDriver: Intel HEVC registration failed (%d)",
                        (int)status);
                }
                atomic_store_explicit(&WEXRegistrationState, 2,
                                      memory_order_release);
            } else {
                /* A later Metal device initialization may be the first point
                 * at which the accelerator is fully visible. Releasing state
                 * is nonblocking and never re-enters a registry callback. */
                atomic_store_explicit(&WEXRegistrationState, 0,
                                      memory_order_release);
            }
        }
    }
    return initialized;
}

static BOOL WEXHasExactSelectedIntelAccelerator(void) {
    io_iterator_t iterator = IO_OBJECT_NULL;
    kern_return_t result = IOServiceGetMatchingServices(kIOMainPortDefault,
        IOServiceMatching("IntelAccelerator"), &iterator);
    if (result != KERN_SUCCESS || iterator == IO_OBJECT_NULL) return NO;

    unsigned selectedCount = 0;
    unsigned exactCount = 0;
    io_service_t accelerator;
    while ((accelerator = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        CFTypeRef ioClass = IORegistryEntryCreateCFProperty(accelerator,
            CFSTR("IOClass"), kCFAllocatorDefault, 0);
        CFTypeRef pluginName = IORegistryEntryCreateCFProperty(accelerator,
            CFSTR("MetalPluginName"), kCFAllocatorDefault, 0);
        CFTypeRef pluginClass = IORegistryEntryCreateCFProperty(accelerator,
            CFSTR("MetalPluginClassName"), kCFAllocatorDefault, 0);
        BOOL selected = ioClass && CFGetTypeID(ioClass) == CFStringGetTypeID() &&
            CFStringCompare((CFStringRef)ioClass, CFSTR("IntelAccelerator"), 0) == kCFCompareEqualTo &&
            pluginName && CFGetTypeID(pluginName) == CFStringGetTypeID() &&
            CFStringCompare((CFStringRef)pluginName,
                            (__bridge CFStringRef)WEXSelectedPluginName, 0) == kCFCompareEqualTo;
        BOOL selectedClassMatches = !pluginClass ||
            (CFGetTypeID(pluginClass) == CFStringGetTypeID() &&
             CFStringCompare((CFStringRef)pluginClass,
                             (__bridge CFStringRef)WEXOwnPrincipalName, 0) == kCFCompareEqualTo);
        selected = selected && selectedClassMatches;
        if (selected) {
            ++selectedCount;
            BOOL exactIdentity = NO;
            io_registry_entry_t node = accelerator;
            IOObjectRetain(node);
            for (unsigned depth = 0; depth < 6 && node != IO_OBJECT_NULL; ++depth) {
                CFTypeRef vendor = IORegistryEntryCreateCFProperty(node,
                    CFSTR("vendor-id"), kCFAllocatorDefault, 0);
                CFTypeRef device = IORegistryEntryCreateCFProperty(node,
                    CFSTR("device-id"), kCFAllocatorDefault, 0);
                if (vendor && device) {
                    uint32_t vendorID = 0, deviceID = 0;
                    if (CFGetTypeID(vendor) == CFDataGetTypeID() &&
                        CFDataGetLength((CFDataRef)vendor) == 4 &&
                        CFGetTypeID(device) == CFDataGetTypeID() &&
                        CFDataGetLength((CFDataRef)device) == 4) {
                        const UInt8 *v = CFDataGetBytePtr((CFDataRef)vendor);
                        const UInt8 *d = CFDataGetBytePtr((CFDataRef)device);
                        vendorID = (uint32_t)v[0] | ((uint32_t)v[1] << 8) |
                                   ((uint32_t)v[2] << 16) | ((uint32_t)v[3] << 24);
                        deviceID = (uint32_t)d[0] | ((uint32_t)d[1] << 8) |
                                   ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
                        exactIdentity = vendorID == WEXTargetVendorID &&
                                        deviceID == WEXTargetDeviceID;
                    }
                }
                if (vendor) CFRelease(vendor);
                if (device) CFRelease(device);
                if (exactIdentity) break;
                io_registry_entry_t parent = IO_OBJECT_NULL;
                if (IORegistryEntryGetParentEntry(node, kIOServicePlane, &parent) != KERN_SUCCESS ||
                    parent == IO_OBJECT_NULL) break;
                IOObjectRelease(node);
                node = parent;
            }
            if (node != IO_OBJECT_NULL) IOObjectRelease(node);
            if (exactIdentity) ++exactCount;
        }
        if (ioClass) CFRelease(ioClass);
        if (pluginName) CFRelease(pluginName);
        if (pluginClass) CFRelease(pluginClass);
        IOObjectRelease(accelerator);
    }
    IOObjectRelease(iterator);
    return selectedCount == 1 && exactCount == 1;
}

BOOL WEXMetalEntryPreparePrincipalClass(NSError **errorOut) {
    __block BOOL prepared = NO;
    dispatch_once(&WEXPrincipalSetupOnce, ^{
        @autoreleasepool {
            NSBundle *nativeBundle = nil;
            NSError *error = nil;
            if (!WEXNativeBundleMetadataIsExact(&nativeBundle, NULL, &error)) {
                WEXPrincipalSetupError = error ?: WEXEntryError(6, @"The native Metal bundle failed identity checks");
                return;
            }
            if (![nativeBundle loadAndReturnError:&error]) {
                WEXPrincipalSetupError = error ?: WEXEntryError(7, @"The installed native TGL Metal driver could not be loaded");
                return;
            }

            Class nativeClass = NSClassFromString(WEXNativePrincipalName);
            SEL initializer = sel_registerName("initWithAcceleratorPort:");
            Method nativeInitializer = nativeClass ? class_getInstanceMethod(nativeClass, initializer) : NULL;
            if (!nativeClass || !WEXMethodHasAcceleratorPortABI(nativeInitializer)) {
                WEXPrincipalSetupError = WEXEntryError(8, @"The native TGL principal class does not expose the verified initWithAcceleratorPort: ABI");
                return;
            }

            SEL graphicsAnchor = sel_registerName("newBufferWithLength:options:");
            Method nativeGraphicsMethod = class_getInstanceMethod(nativeClass, graphicsAnchor);
            Dl_info nativeImage = {0};
            if (!nativeGraphicsMethod ||
                !dladdr((const void *)method_getImplementation(nativeGraphicsMethod), &nativeImage) ||
                !nativeImage.dli_fname ||
                ![[[NSString stringWithUTF8String:nativeImage.dli_fname] stringByResolvingSymlinksInPath]
                    isEqualToString:[nativeBundle.executablePath stringByResolvingSymlinksInPath]]) {
                WEXPrincipalSetupError = WEXEntryError(12, @"The native TGL Metal allocation anchor is not implemented by the pinned driver executable");
                return;
            }

            Class wrapperClass = objc_allocateClassPair(nativeClass,
                WEXOwnPrincipalName.UTF8String, 0);
            if (!wrapperClass) {
                wrapperClass = NSClassFromString(WEXOwnPrincipalName);
                if (!wrapperClass || class_getSuperclass(wrapperClass) != nativeClass) {
                    WEXPrincipalSetupError = WEXEntryError(9, @"Could not create the private WEX TGL principal subclass");
                    return;
                }
            } else {
                if (!class_addMethod(wrapperClass, initializer,
                        (IMP)WEXInitWithAcceleratorPort,
                        method_getTypeEncoding(nativeInitializer))) {
                    objc_disposeClassPair(wrapperClass);
                    WEXPrincipalSetupError = WEXEntryError(10, @"Could not install the WEX initializer on its own subclass");
                    return;
                }
                objc_registerClassPair(wrapperClass);
            }

            Method wrapperInitializer = class_getInstanceMethod(wrapperClass, initializer);
            Method wrapperGraphicsMethod = class_getInstanceMethod(wrapperClass, graphicsAnchor);
            if (!wrapperInitializer || !WEXMethodHasAcceleratorPortABI(wrapperInitializer) ||
                !wrapperGraphicsMethod ||
                method_getImplementation(wrapperGraphicsMethod) != method_getImplementation(nativeGraphicsMethod) ||
                class_getSuperclass(wrapperClass) != nativeClass) {
                WEXPrincipalSetupError = WEXEntryError(11, @"The WEX principal subclass failed its inherited ABI check");
                return;
            }

            WEXNativeDeviceClass = nativeClass;
            WEXPrincipalDeviceClass = wrapperClass;
            prepared = YES;
        }
    });
    if (!prepared) {
        prepared = WEXPrincipalDeviceClass != Nil;
        if (errorOut && !prepared) *errorOut = WEXPrincipalSetupError;
    }
    return prepared;
}

/* The host process resolves the principal class after loading this bundle.
 * Prepare it before NSBundle asks for NSPrincipalClass. */
__attribute__((constructor))
static void WEXMetalBundleLoad(void) {
    @autoreleasepool {
        NSError *error = nil;
        if (!WEXMetalEntryPreparePrincipalClass(&error)) {
            NSLog(@"WEXHEVCMetalDriver: principal-class setup failed: %@", error);
        }
    }
}
