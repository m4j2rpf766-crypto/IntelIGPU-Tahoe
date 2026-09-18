#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOCatalogue.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSUnserialize.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSBoolean.h>
#include "profiles.hpp"

static const char *const kPCIPath="IOService:/AppleACPIPlatformExpert/PC00/AppleACPIPCI/GFX0@2";
static const char *const kTag="ReimsManualSessionPersonality";
static bool isolatedADLP(IOPCIDevice *pci) {
    if (!pci || pci->isInactive()) return false;
    char path[512]; int size=sizeof(path);
    if (!pci->getPath(path,&size,gIOServicePlane) || strcmp(path,kPCIPath)) return false;
    auto *device=OSDynamicCast(OSData,pci->getProperty("device-id"));
    const unsigned char isolated[]={0xff,0xff,0,0};
    return device && device->getLength()==sizeof(isolated) &&
        !memcmp(device->getBytesNoCopy(),isolated,sizeof(isolated)) &&
        pci->configRead16(kIOPCIConfigVendorID)==0x8086 &&
        pci->configRead16(kIOPCIConfigDeviceID)==0x46a3 &&
        pci->configRead8(kIOPCIConfigRevisionID)==0x0c;
}
static IOPCIDevice *copyTarget() {
    auto *entry=IORegistryEntry::fromPath(kPCIPath,nullptr);
    auto *pci=OSDynamicCast(IOPCIDevice,entry);
    if (!isolatedADLP(pci)) { OSSafeReleaseNULL(entry); return nullptr; }
    return pci; // Own the fromPath reference.
}
static IOReturn addProfiles(const char *xml) {
    auto object=OSUnserializeXML(xml);
    auto *profiles=OSDynamicCast(OSArray,object);
    if (!profiles || !gIOCatalogue) return kIOReturnInternalError;
    // Never accept caller-supplied personalities or alter persistent caches.
    return gIOCatalogue->addDrivers(profiles,true)?kIOReturnSuccess:kIOReturnError;
}
class ReimsADLManualMatchProbe : public IOService {
    OSDeclareDefaultStructors(ReimsADLManualMatchProbe)
public:
    bool start(IOService *provider) override {
        if (!isolatedADLP(OSDynamicCast(IOPCIDevice,provider))) return false;
        if (!IOService::start(provider)) return false;
        setProperty("PhysicalIdentityVerified",true);
        setProperty("GPUAccess","none: PCI identity reads only");
        registerService(); return true;
    }
};
OSDefineMetaClassAndStructors(ReimsADLManualMatchProbe,IOService)

class ReimsADLManualActivation : public IOService {
    OSDeclareDefaultStructors(ReimsADLManualActivation)
    IOLock *lock=nullptr;
    bool runtimeRequested=false;
    bool displayCommitted=false;
    IOService *link=nullptr;
    static IOService *copyChild(IOService *parent,const char *className) {
        auto *iterator=parent->getChildIterator(gIOServicePlane);
        if (!iterator) return nullptr;
        IOService *found=nullptr;
        while (auto *object=iterator->getNextObject()) {
            auto *child=OSDynamicCast(IOService,object);
            if (child && !child->isInactive() && child->metaCast(className)) {
                if (found) { found->release(); iterator->release(); return nullptr; }
                found=child; found->retain();
            }
        }
        iterator->release(); return found;
    }
    IOReturn linkAction(const char *name) {
        auto *dict=OSDictionary::withCapacity(1);
        if (!dict) return kIOReturnNoMemory;
        dict->setObject(name,kOSBooleanTrue);
        IOReturn result=link->setProperties(dict);
        dict->release(); return result;
    }
    IOReturn prepareLink(IOPCIDevice *pci) {
        if (!runtimeRequested || displayCommitted) return kIOReturnNotReady;
        auto *accel=copyChild(pci,"IntelAccelerator");
        if (!accel) return kIOReturnNotReady;
        UInt32 state=accel->getState();
        if (pci->getBusyState()!=0 || (state&(kIOServiceRegisteredState|kIOServiceInactiveState))) {
            accel->release(); return kIOReturnNotReady;
        }
        if (!link) {
            auto *existing=copyChild(accel,"ReimsADLDesktopLink");
            if (existing) { existing->release(); accel->release(); return kIOReturnExclusiveAccess; }
            auto profile=OSUnserializeXML(link_profile);
            auto *dict=OSDynamicCast(OSDictionary,profile);
            // IOKit matches categories by interned OSSymbol identity. Reserve
            // the same category before publishing the accelerator, so normal
            // catalogue matching cannot create a second DesktopLink instance.
            auto *category=OSSymbol::withCString("ReimsADLDesktopLink");
            if (!dict || !category) { OSSafeReleaseNULL(category); accel->release(); return kIOReturnNoMemory; }
            bool categoryOK=dict->setObject("IOMatchCategory",category);
            category->release();
            if (!categoryOK) { accel->release(); return kIOReturnNoMemory; }
            auto *object=OSMetaClass::allocClassWithName("ReimsADLDesktopLink");
            auto *candidate=OSDynamicCast(IOService,object);
            if (!candidate || !dict || !candidate->init(dict)) {
                OSSafeReleaseNULL(object); accel->release(); return kIOReturnNotReady;
            }
            if (!candidate->attach(accel)) { candidate->release(); accel->release(); return kIOReturnError; }
            if (!candidate->start(accel)) {
                candidate->detach(accel); candidate->release(); accel->release(); return kIOReturnError;
            }
            link=candidate;
        }
        accel->release();
        IOReturn result=linkAction("PrepareRCSADLP");
        if (result==kIOReturnSuccess && link->getProperty("RCSADLPWorkaroundLive")!=kOSBooleanTrue)
            result=kIOReturnNotReady;
        setProperty("DisplayPreparationReady",result==kIOReturnSuccess);
        return result;
    }
    IOReturn commitDisplay(IOPCIDevice *pci) {
        if (!link || displayCommitted || getProperty("DisplayPreparationReady")!=kOSBooleanTrue)
            return kIOReturnNotReady;
        auto *accel=link->getProvider();
        if (!accel || accel->getProvider()!=pci || accel->isInactive() ||
            (accel->getState()&kIOServiceRegisteredState)) return kIOReturnNotReady;
        IOReturn result=linkAction("NativeFramebuffer");
        if (result!=kIOReturnSuccess) return result;
        auto *fb=copyChild(pci,"ReimsIntelADLFramebuffer");
        bool ready=fb && link->getProperty("Linked")==kOSBooleanTrue &&
            link->getProperty("NativeFramebuffer")==kOSBooleanTrue &&
            link->getProperty("RCSADLPWorkaroundLive")==kOSBooleanTrue;
        OSSafeReleaseNULL(fb);
        if (!ready) return kIOReturnNotReady;
        displayCommitted=true;
        setProperty("DisplayCommitted",true);
        // Publication is last. This is NOT a GPU completion/fence substitute.
        accel->registerService();
        return kIOReturnSuccess;
    }
    bool selfTestRequested=false;
    IOReturn removeProfiles() {
        auto *matching=OSDictionary::withCapacity(1);
        if (!matching) return kIOReturnNoMemory;
        matching->setObject(kTag,kOSBooleanTrue);
        // Removing a personality does not terminate an active driver.
        bool ok=gIOCatalogue && gIOCatalogue->removeDrivers(matching,false);
        matching->release(); return ok?kIOReturnSuccess:kIOReturnError;
    }
public:
    bool start(IOService *provider) override {
        if (!IOService::start(provider)) return false;
        lock=IOLockAlloc(); if (!lock) return false;
        setProperty("ActivationPolicy","explicit-root-current-boot-only");
        setProperty("RuntimeRequested",false);
        setProperty("SelfTestRequested",false);
        setProperty("DisplayHandoffVerified",false);
        // Loading/boot matching this service never matches the GPU.
        registerService(); return true;
    }
    IOReturn setProperties(OSObject *properties) override {
        if (IOUserClient::clientHasPrivilege(current_task(),kIOClientPrivilegeAdministrator)!=kIOReturnSuccess)
            return kIOReturnNotPrivileged;
        auto *dict=OSDynamicCast(OSDictionary,properties);
        if (!dict || dict->getCount()!=1 || !lock) return kIOReturnBadArgument;
        auto *action=OSDynamicCast(OSString,dict->getObject("ManualAction"));
        if (!action) return kIOReturnBadArgument;
        IOLockLock(lock);
        IOReturn result=kIOReturnBadArgument;
        if (action->isEqualTo("RemoveSessionRules")) {
            result=removeProfiles();
            // Do not falsely claim the active driver was stopped.
            setProperty("SessionRulesRemoved",result==kIOReturnSuccess);
        } else if (action->isEqualTo("SelfTest")) {
            auto *pci=copyTarget();
            if (!pci) result=kIOReturnUnsupported;
            else if (selfTestRequested) result=kIOReturnExclusiveAccess;
            else {
                selfTestRequested=true;
                result=addProfiles(selftest_profiles);
                if (result!=kIOReturnSuccess) selfTestRequested=false;
                setProperty("SelfTestRequested",selfTestRequested);
            }
            OSSafeReleaseNULL(pci);
        } else if (action->isEqualTo("PublishRuntime")) {
            // This name remains rejected: callers must use the staged protocol.
            result=kIOReturnUnsupported;
        } else if (action->isEqualTo("PrepareRuntime")) {
            auto *pci=copyTarget();
            auto *probe=pci?copyChild(pci,"ReimsADLManualMatchProbe"):nullptr;
            auto *accel=pci?copyChild(pci,"IntelAccelerator"):nullptr;
            if (!pci || !probe || probe->getProperty("PhysicalIdentityVerified")!=kOSBooleanTrue)
                result=kIOReturnNotReady;
            else if (runtimeRequested || accel) result=kIOReturnExclusiveAccess;
            else {
                runtimeRequested=true;
                result=addProfiles(runtime_profiles);
                // An error after matching began is not a safe retry condition.
                setProperty("RuntimeRequested",true);
            }
            OSSafeReleaseNULL(accel); OSSafeReleaseNULL(probe); OSSafeReleaseNULL(pci);
        } else if (action->isEqualTo("PrepareDisplay") || action->isEqualTo("CommitDisplay")) {
            auto *pci=copyTarget();
            if (!pci) result=kIOReturnUnsupported;
            else result=action->isEqualTo("PrepareDisplay")?prepareLink(pci):commitDisplay(pci);
            OSSafeReleaseNULL(pci);
        }
        setProperty("LastResult",static_cast<UInt64>(static_cast<UInt32>(result)),32);
        IOLockUnlock(lock); return result;
    }
    void stop(IOService *provider) override {
        if (lock) { IOLockLock(lock); removeProfiles(); IOLockUnlock(lock); }
        // Active framebuffer teardown must use the prepared reboot rollback;
        // do not tear down resources that scanout may still own here.
        IOService::stop(provider);
    }
    void free() override {
        if (lock) { IOLockFree(lock); lock=nullptr; }
        OSSafeReleaseNULL(link);
        IOService::free();
    }
};
OSDefineMetaClassAndStructors(ReimsADLManualActivation,IOService)
