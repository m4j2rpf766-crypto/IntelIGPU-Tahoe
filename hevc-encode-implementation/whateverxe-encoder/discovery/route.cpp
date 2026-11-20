#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/version.h>
#include <string.h>

namespace {
constexpr char kExpectedOSBuild[] = "25G422";
constexpr char kTGLDriverBundle[] = "com.apple.driver.AppleIntelTGLGraphics";
constexpr char kTGLMetalBundle[] = "AppleIntelTGLGraphicsMTLDriver";
constexpr char kWEXMetalBundle[] = "WEXHEVCMetalDriver";
constexpr uint32_t kTGLRendererID = 0x01080080;
constexpr char kMetalPluginName[] = "MetalPluginName";
constexpr char kMetalPluginClassName[] = "MetalPluginClassName";

class RetainedProperty final {
    OSObject *object;

public:
    explicit RetainedProperty(OSObject *value) : object(value) {}
    ~RetainedProperty() {
        if (object)
            object->release();
    }

    RetainedProperty(const RetainedProperty &) = delete;
    RetainedProperty &operator=(const RetainedProperty &) = delete;

    OSObject *get() const { return object; }
    OSObject *release() {
        auto *value = object;
        object = nullptr;
        return value;
    }
};

bool matchesSupportedUHD770(IOService *accelerator) {
    if (!accelerator || strcmp(osversion, kExpectedOSBuild) != 0)
        return false;

    auto pci = OSDynamicCast(IOPCIDevice, accelerator->getProvider());
    if (!pci || pci->isInactive() ||
        pci->configRead16(kIOPCIConfigVendorID) != 0x8086 ||
        pci->configRead16(kIOPCIConfigDeviceID) != 0x4680 ||
        !(pci->configRead16(kIOPCIConfigCommand) & 0x0002))
        return false;

    RetainedProperty driverBundleValue(
        accelerator->copyProperty("CFBundleIdentifier"));
    RetainedProperty kernelBundleValue(
        accelerator->copyProperty("CFBundleIdentifierKernel"));
    RetainedProperty rendererIDValue(
        accelerator->copyProperty("IOVARendererID"));
    auto driverBundle = OSDynamicCast(OSString, driverBundleValue.get());
    auto kernelBundle = OSDynamicCast(OSString, kernelBundleValue.get());
    auto rendererID = OSDynamicCast(OSNumber, rendererIDValue.get());

    return driverBundle && kernelBundle && rendererID &&
        driverBundle->isEqualTo(kTGLDriverBundle) &&
        kernelBundle->isEqualTo(kTGLDriverBundle) &&
        rendererID->unsigned32BitValue() == kTGLRendererID;
}
}

class WEXIntelHEVCMetalRoute final : public IOService {
    OSDeclareDefaultStructors(WEXIntelHEVCMetalRoute)

    IOService *accelerator = nullptr;
    OSObject *originalMetalPluginName = nullptr;
    OSString *publishedMetalPluginName = nullptr;
    bool ownsMetalPluginName = false;

    bool installRoute(IOService *target) {
        RetainedProperty replacementValue(OSString::withCString(kWEXMetalBundle));
        auto replacement = OSDynamicCast(OSString, replacementValue.get());
        if (!replacement)
            return false;

        // Serialize this route's lifecycle. Retained property snapshots keep
        // values alive even if another publisher changes the registry entry.
        if (!target->lockForArbitration())
            return false;

        RetainedProperty originalValue(target->copyProperty(kMetalPluginName));
        RetainedProperty classNameValue(
            target->copyProperty(kMetalPluginClassName));
        auto original = OSDynamicCast(OSString, originalValue.get());
        const bool stillNative = original && original->isEqualTo(kTGLMetalBundle) &&
            !classNameValue.get();
        if (stillNative) {
            if (target->setProperty(kMetalPluginName, replacement)) {
                originalMetalPluginName = originalValue.release();
                publishedMetalPluginName = replacement;
                replacementValue.release();
                ownsMetalPluginName = true;
            }
        }

        RetainedProperty currentValue(target->copyProperty(kMetalPluginName));
        const bool installed = ownsMetalPluginName &&
            currentValue.get() == publishedMetalPluginName;
        target->unlockForArbitration();
        return installed;
    }

    void restoreRoute(IOService *target) {
        if (!ownsMetalPluginName || !target)
            return;

        if (!target->lockForArbitration())
            return;

        // Never restore over a value that no longer belongs to this instance.
        // IORegistryEntry exposes synchronized individual property operations,
        // but no compare-and-swap API; checking while holding provider
        // arbitration also serializes duplicate lifecycle callbacks here.
        RetainedProperty currentValue(target->copyProperty(kMetalPluginName));
        if (currentValue.get() == publishedMetalPluginName &&
            !target->setProperty(kMetalPluginName, originalMetalPluginName)) {
            IOLog("WEXIntelHEVCMetalRoute: could not restore MetalPluginName\n");
        }

        target->unlockForArbitration();
        ownsMetalPluginName = false;
        OSSafeReleaseNULL(publishedMetalPluginName);
        OSSafeReleaseNULL(originalMetalPluginName);
    }

public:
    bool start(IOService *provider) override {
        if (!matchesSupportedUHD770(provider) || !IOService::start(provider))
            return false;

        if (!installRoute(provider)) {
            restoreRoute(provider);
            IOService::stop(provider);
            return false;
        }

        accelerator = provider;
        accelerator->retain();
        registerService();
        return true;
    }

    void stop(IOService *provider) override {
        restoreRoute(provider);
        OSSafeReleaseNULL(accelerator);
        IOService::stop(provider);
    }

    void free() override {
        OSSafeReleaseNULL(publishedMetalPluginName);
        OSSafeReleaseNULL(originalMetalPluginName);
        OSSafeReleaseNULL(accelerator);
        IOService::free();
    }
};

OSDefineMetaClassAndStructors(WEXIntelHEVCMetalRoute, IOService)
