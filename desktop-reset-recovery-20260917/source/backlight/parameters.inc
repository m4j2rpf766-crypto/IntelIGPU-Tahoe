#include <IOKit/graphics/IODisplay.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSBoolean.h>
#include "../backlight/pwm.hpp"

// Intel ADL-P PCH PWM0. Preserve the firmware's period, enable, polarity and
// panel-power sequencing; only the duty register is changed. See Linux i915
// intel_backlight.c bxt_set_backlight/cnp_setup_backlight.
class ReimsADLBacklightParameters : public IODisplayParameterHandler {
 OSDeclareDefaultStructors(ReimsADLBacklightParameters)
 IOMemoryMap *mmio=nullptr;
 IOLock *lock=nullptr;
 const OSSymbol *brightness=nullptr;
 const OSSymbol *linear=nullptr,*userProbe=nullptr,*linearProbe=nullptr;
 // Retain the parameter dictionaries, not the display (which retains us).
 // Other IODisplay handlers shallow-merge these dictionaries. There is no
 // display lifetime dependency or dangling pointer after display retirement.
 OSDictionary *displayParameters=nullptr;
 uint32_t period=0,control=0;
 uint64_t writes=0,probes=0;
 uint32_t read(uint32_t off)const{return *reinterpret_cast<volatile uint32_t*>(mmio->getVirtualAddress()+off);}
 bool hardwareValid()const{return mmio&&read(0xc8250)==control&&read(0xc8254)==period;}
 void snapshot(){
  setProperty("PWMControl",uint64_t(read(0xc8250)),32);
  setProperty("PWMPeriod",uint64_t(read(0xc8254)),32);
  setProperty("PWMDuty",uint64_t(read(0xc8258)),32);
  setProperty("MinimumPWMDuty",uint64_t(ReimsPWM::minimumPanelDuty(period)),32);
  setProperty("BrightnessWrites",writes,64);
  setProperty("ProbeCalls",probes,64);
 }
public:
 bool configure(IOMemoryMap*m){
  if(!init()||!m||m->getLength()<0xc835c)return false;
  mmio=m;m->retain();lock=IOLockAlloc();brightness=OSSymbol::withCString(kIODisplayBrightnessKey);
  linear=OSSymbol::withCString(kIODisplayLinearBrightnessKey);
  userProbe=OSSymbol::withCString(kIODisplayBrightnessProbeKey);
  linearProbe=OSSymbol::withCString(kIODisplayLinearBrightnessProbeKey);
  if(!lock||!brightness||!linear||!userProbe||!linearProbe)return false;
  control=read(0xc8250);period=read(0xc8254);
  // Exact observed controller: native PWM0, no second enabled controller.
  if(!ReimsPWM::valid(control,period,read(0xc8258),read(0xc8350)))return false;
  setProperty("BacklightImplementation","adl-p-pch-pwm0-duty-only");snapshot();return true;
 }
 bool setDisplay(IODisplay*d) override {
  if(!d){IOLockLock(lock);OSSafeReleaseNULL(displayParameters);IOLockUnlock(lock);return true;}
  // Only bind displays below this same physical Intel device. A global
  // backlight service must never provide controls for an external GPU.
  IORegistryEntry*ancestor=d;bool ours=false;
  while(ancestor){
   if(auto*p=OSDynamicCast(IOPCIDevice,ancestor)){
    ours=p==getProvider()->getProvider();break;
   }
   ancestor=ancestor->getParentEntry(gIOServicePlane);
  }
  if(!ours)return false;
  IOLockLock(lock);
  const bool valid=hardwareValid();const uint32_t duty=read(0xc8258);
  IOLockUnlock(lock);if(!valid||duty>period)return false;
  auto*old=d->copyProperty(kIODisplayParametersKey);
  auto*dict=OSDynamicCast(OSDictionary,old);
  auto*merged=dict?OSDictionary::withDictionary(dict):OSDictionary::withCapacity(1);
  OSSafeReleaseNULL(old);if(!merged)return false;
  bool ok=true;
  const OSSymbol*keys[]={brightness,linear,userProbe,linearProbe};
  for(auto*key:keys){
   const bool legacy=key==brightness||key==userProbe;
   ok=ok&&IODisplay::addParameter(merged,key,0,legacy?255:ReimsPWM::linearMax)&&
    IODisplay::setParameter(merged,key,legacy?ReimsPWM::toUser(duty,period):ReimsPWM::dutyToLinear(duty,period));
  }
  ok=ok&&d->setProperty(kIODisplayParametersKey,merged);
  if(ok){IOLockLock(lock);OSSafeReleaseNULL(displayParameters);displayParameters=merged;merged->retain();IOLockUnlock(lock);}
  merged->release();
  if(ok){d->setProperty(kIODisplayHasBacklightKey,true);setProperty("DisplayBound",true);}
  return ok;
 }
 bool doIntegerSet(OSDictionary*params,const OSSymbol*key,UInt32 value) override {
  (void)params; // IODisplay supplies the single key's range, not the root dictionary.
  const bool probe=key==userProbe||key==linearProbe;
  const bool legacy=key==brightness||key==userProbe;
  if(!probe&&key!=brightness&&key!=linear)return false;
  if(value>(legacy?255:ReimsPWM::linearMax)||!mmio)return false;
  IOLockLock(lock);
  if(!hardwareValid()||!displayParameters){IOLockUnlock(lock);return false;}
  const uint32_t linearValue=legacy?ReimsPWM::userToLinear(value):value;
  const uint32_t userValue=legacy?value:ReimsPWM::linearToUser(value);
  // CoreDisplay GetUserBrightness writes the linear probe then reads the
  // user probe. This is a conversion query: never change hardware or actual
  // brightness here. Our documented curve is normalized identity, not a
  // fabricated panel luminance calibration.
  if(probe){
   const bool ok=IODisplay::setParameter(displayParameters,userProbe,userValue)&&
    IODisplay::setParameter(displayParameters,linearProbe,linearValue);
   if(ok)++probes;snapshot();IOLockUnlock(lock);return ok;
  }
  // Clamp only the final hardware output; probes continue to convert logical
  // user values without changing illumination. Preserve all values above 1%.
  const uint32_t duty=ReimsPWM::panelDuty(linearValue,period);
  *reinterpret_cast<volatile uint32_t*>(mmio->getVirtualAddress()+0xc8258)=duty;
  OSSynchronizeIO();
  const bool ok=read(0xc8258)==duty;
  if(ok){
   ++writes;setProperty("RequestedBrightness",uint64_t(userValue),32);
   setProperty("RequestedLinearBrightness",uint64_t(linearValue),32);
   setProperty("LastBrightnessParameter",key->getCStringNoCopy());
   IODisplay::setParameter(displayParameters,brightness,userValue);
   IODisplay::setParameter(displayParameters,linear,linearValue);
  }
  snapshot();IOLockUnlock(lock);
  return ok;
 }
 bool doDataSet(const OSSymbol*,OSData*) override{return false;}
 bool doUpdate() override{return true;}
 IOReturn setProperties(OSObject*o) override {
  auto*d=OSDynamicCast(OSDictionary,o);
  if(!d||d->getObject("CaptureBacklight")!=kOSBooleanTrue)return kIOReturnUnsupported;
  if(IOUserClient::clientHasPrivilege(current_task(),kIOClientPrivilegeAdministrator)!=kIOReturnSuccess)return kIOReturnNotPrivileged;
  IOLockLock(lock);snapshot();IOLockUnlock(lock);return kIOReturnSuccess;
 }
 void free() override {
  OSSafeReleaseNULL(mmio);OSSafeReleaseNULL(brightness);
  OSSafeReleaseNULL(linear);OSSafeReleaseNULL(userProbe);OSSafeReleaseNULL(linearProbe);
  OSSafeReleaseNULL(displayParameters);
  if(lock)IOLockFree(lock);IODisplayParameterHandler::free();
 }
};
OSDefineMetaClassAndStructors(ReimsADLBacklightParameters,IODisplayParameterHandler)

// Attach before publishing/replacing the native framebuffer. The early marker
// is deliberately independent of IOGraphics, which resides in the System KC.
static bool ReimsAttachBacklight(IOService*accelerator){
 auto*matching=IOService::nameMatching("backlight");if(!matching)return false;
 auto*it=IOService::getMatchingServices(matching);matching->release();if(!it)return false;
 bool ok=false;
 while(auto*o=it->getNextObject()){
  auto*marker=OSDynamicCast(IOService,o);
  if(!marker||!marker->metaCast("ReimsADLBacklight")||marker->getProvider()!=accelerator->getProvider())continue;
  auto*children=marker->getChildIterator(gIOServicePlane);
  if(children){while(auto*c=children->getNextObject())if(c->metaCast("ReimsADLBacklightParameters"))ok=true;children->release();}
  if(ok)break;
  auto*p=OSDynamicCast(IOPCIDevice,marker->getProvider());if(!p)break;
  auto*m=p->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0,kIOMapInhibitCache);if(!m)break;
  auto*h=new ReimsADLBacklightParameters;
  ok=h&&h->configure(m);m->release();
  if(ok){ok=h->attach(marker);if(ok&&!h->start(marker)){h->detach(marker);ok=false;}}
  if(ok)h->registerService();OSSafeReleaseNULL(h);break;
 }
 it->release();return ok;
}
