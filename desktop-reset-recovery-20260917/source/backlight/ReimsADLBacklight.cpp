#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOLib.h>
#include "pwm.hpp"
// This early marker has no dependency on IOGraphicsFamily (System KC).
// DesktopLink attaches the actual IODisplayParameterHandler before handoff.
class ReimsADLBacklight : public IOService {
 OSDeclareDefaultStructors(ReimsADLBacklight)
public:
 bool start(IOService*provider) override {
  if(!IOService::start(provider))return false;
  auto*p=OSDynamicCast(IOPCIDevice,provider);
  if(!p||p->configRead16(kIOPCIConfigVendorID)!=0x8086||p->configRead16(kIOPCIConfigDeviceID)!=0x46a3)return false;
  auto*m=p->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0,kIOMapInhibitCache);
  if(!m)return false;
  bool ok=false;
  if(m->getLength()>=0xc835c){
   auto read=[m](uint32_t off){return *reinterpret_cast<volatile uint32_t*>(m->getVirtualAddress()+off);};
   const uint32_t ctl=read(0xc8250),period=read(0xc8254),duty=read(0xc8258);
   ok=ReimsPWM::valid(ctl,period,duty,read(0xc8350));
   if(ok){setProperty("PWMPeriod",uint64_t(period),32);setProperty("BootPWMDuty",uint64_t(duty),32);}
  }
  m->release();if(!ok)return false;
  setName("backlight");setProperty("IODisplayHasBacklight",true);
  setProperty("BacklightStage","early-hardware-marker");registerService();return true;
 }
};
OSDefineMetaClassAndStructors(ReimsADLBacklight,IOService)
