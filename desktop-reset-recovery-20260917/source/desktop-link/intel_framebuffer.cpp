#include "intel_framebuffer.hpp"
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/graphics/IODisplay.h>
#include <IOKit/ndrvsupport/IOMacOSVideo.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSArray.h>
#include "native_transaction.hpp"
#include "native_mapping.hpp"
#include "native_layout.hpp"
#include "flip_state.hpp"
#include "display_timing.hpp"
#include <kern/clock.h>
#include "flip_profile.hpp"

// Each standard IOFramebuffer interrupt registration owns its callback target.
// Both IOGraphics and IOAccelDisplayPipe subscribe to 'vbl '; they must not
// replace or disable each other's registration. Snapshots retain these objects
// while callbacks run outside the framebuffer state lock.
class ReimsVBLRegistration : public OSObject {
 OSDeclareDefaultStructors(ReimsVBLRegistration)
public:
 bool init() override {return OSObject::init();}
 IOFBInterruptProc proc=nullptr;
 OSObject*target=nullptr;
 void*ref=nullptr;
 bool enabled=false,registered=true;
 void free() override {OSSafeReleaseNULL(target);OSObject::free();}
};
OSDefineMetaClassAndStructors(ReimsVBLRegistration,OSObject)

// Actual IOFramebuffer subclass: CoreDisplay determines its Intel display
// family from the class name. No vendor mask or compositor preference patch.
// Initially preserves the firmware-programmed pipe, link and backlight.
class ReimsIntelADLFramebuffer : public IOFramebuffer {
 OSDeclareDefaultStructors(ReimsIntelADLFramebuffer)
 IODeviceMemory *aperture=nullptr;
 IOMemoryMap *registers=nullptr;
 IOService *accelerator=nullptr;
 IOPixelInformation pixel={};
 IODisplayModeInformation modeInfo={};
 IOTimingInformation timing={};
 IOTimerEventSource *timer=nullptr;
 IOWorkLoop *completionWorkLoop=nullptr;
 OSArray *vblRegistrations=nullptr; // copy-on-write; tick never mutates a snapshot
 IORecursiveLock *stateLock=nullptr;
 bool ready=false;
 uint32_t hardwarePowerState=2;
 uint64_t vblCallbacks=0,nativeVBLCallbacks=0;
 uint32_t lastFrame=0, platformCalls=0;
 uint64_t wraps=0,notifiedWraps=0;
 uint64_t validations=0,flips=0;
 ReimsFlip::State flipState;
 OSObject *frontResource=nullptr,*pendingResource=nullptr;
 IOSurface *frontSurface=nullptr,*pendingSurface=nullptr;
 uint64_t pinAcquires=0,pinReleases=0,itcmCalls=0;
 uint32_t bootPlaneCtl=0,liveBase=0;
 bool changedDPT=false;
 uint32_t savedDPT=0;
 bool optimizeScanout=true,forceDiagnostics=false,profileEnabled=false;
 struct ProofEntry {OSObject*r=nullptr;IOSurface*s=nullptr;uint64_t age=0;ReimsNativeMapping::PreparedProof proof;};
 ProofEntry proofs[8];uint64_t proofAge=0,proofHits=0,proofMisses=0;
 ReimsNativeMapping::Result lastMapping;
 ReimsNativeLayout::Plan lastLayout;
 ReimsFlipProfile::Record*profile=nullptr;
 unsigned profileCount=0,profileDropped=0,profilePending=UINT32_MAX,profileCurrent=UINT32_MAX;
 static uint64_t nowNS(){uint64_t a=0,n=0;clock_get_uptime(&a);absolutetime_to_nanoseconds(a,&n);return n;}
 template<class...Args> void diag(const char*key,Args...args){
  if(!optimizeScanout||forceDiagnostics)IOService::setProperty(key,args...);
 }
 void clearProofs(){for(auto&e:proofs){e.proof.valid=false;unpinResource(e.r);OSSafeReleaseNULL(e.s);e.age=0;}}
 ReimsNativeMapping::PreparedProof*proofFor(OSObject*r,IOSurface*s){
  if(!optimizeScanout){clearProofs();return nullptr;}
  ProofEntry*choice=nullptr;
  for(auto&e:proofs){
   if(e.r==r&&e.s==s){e.age=++proofAge;return &e.proof;}
   if(!e.r){choice=&e;break;}
   if(e.r!=frontResource&&e.r!=pendingResource&&(!choice||e.age<choice->age))choice=&e;
  }
  if(!choice)return nullptr;
  choice->proof.valid=false;unpinResource(choice->r);OSSafeReleaseNULL(choice->s);
  // Independent extra prepare keeps this cache epoch mapped after the display
  // releases its own front pin. Cache size is not an in-flight limit: misses
  // always use the full checked path. Native transaction gate is held here.
  if(!pinResource(r))return nullptr;
  choice->r=r;choice->s=s;s->retain();choice->age=++proofAge;return &choice->proof;
 }
 ReimsFlipProfile::Record*currentProfile(){return profile&&profileCurrent<profileCount?&profile[profileCurrent]:nullptr;}
 ReimsFlipProfile::Record*pendingProfile(){return profile&&profilePending<profileCount?&profile[profilePending]:nullptr;}
 void publishProfile(){
  if(profile){auto*d=OSData::withBytes(profile,profileCount*sizeof(*profile));if(d){setProperty("ReimsFlipProfileV2",d);d->release();}}
  setProperty("ReimsFlipProfileDropped",uint64_t(profileDropped),32);
  setProperty("ReimsProofCacheHits",proofHits,64);setProperty("ReimsProofCacheMisses",proofMisses,64);
  setProperty("ReimsScanoutOptimized",optimizeScanout);
  forceDiagnostics=true;recordLayout(lastLayout);forceDiagnostics=false;
  setProperty("ReimsMappingResult",uint64_t(lastMapping.code),32);
 }
 ReimsDisplayTiming::Snapshot captureTiming(){
  ReimsDisplayTiming::Snapshot s;
  for(unsigned i=0;i<ReimsDisplayTiming::count;++i)s.value[i]=reg(ReimsDisplayTiming::offsets[i]);
  uint32_t pairs[ReimsDisplayTiming::count*2];
  for(unsigned i=0;i<ReimsDisplayTiming::count;++i){pairs[2*i]=ReimsDisplayTiming::offsets[i];pairs[2*i+1]=s.value[i];}
  auto*d=OSData::withBytes(pairs,sizeof(pairs));if(d){setProperty("ReimsDisplayTimingRegistersV1",d);d->release();}
  uint64_t absolute=0,ns=0;clock_get_uptime(&absolute);absolutetime_to_nanoseconds(absolute,&ns);
  setProperty("ReimsTimingSampleNS",ns,64);
  setProperty("ReimsTimingFrameCounter",uint64_t(reg(0x70040)),32);
  return s;
 }
 bool configureHardwareTiming(){
  const auto s=captureTiming();ReimsDisplayTiming::Mode m;
  const auto status=ReimsDisplayTiming::decode(s,m);
  setProperty("ReimsHardwareTimingStatus",uint64_t(status),32);
  if(status!=ReimsDisplayTiming::Status::OK||m.hActive!=pixel.activeWidth||m.vActive!=pixel.activeHeight)return false;
  bzero(&timing,sizeof(timing));timing.flags=kIODetailedTimingValid;
  auto&t=timing.detailedInfo.v2;
  t.pixelClock=t.minPixelClock=t.maxPixelClock=m.pixelClockHz;
  t.horizontalActive=m.hActive;t.horizontalBlanking=m.hTotal-m.hActive;
  t.horizontalSyncOffset=m.hSyncStart-m.hActive;t.horizontalSyncPulseWidth=m.hSyncEnd-m.hSyncStart;
  t.verticalActive=m.vActive;t.verticalBlanking=m.vTotal-m.vActive;
  t.verticalSyncOffset=m.vSyncStart-m.vActive;t.verticalSyncPulseWidth=m.vSyncEnd-m.vSyncStart;
  t.horizontalSyncConfig=m.hPositive?kIOSyncPositivePolarity:0;
  t.verticalSyncConfig=m.vPositive?kIOSyncPositivePolarity:0;t.numLinks=1;
  modeInfo.refreshRate=m.refresh1616;
  setProperty("ReimsHardwarePixelClockHz",m.pixelClockHz,64);
  setProperty("ReimsHardwarePortClockHz",m.portClockHz,64);
  setProperty("ReimsHardwareRefresh1616",uint64_t(m.refresh1616),32);
  setProperty("ReimsHardwareHTotal",uint64_t(m.hTotal),32);
  setProperty("ReimsHardwareVTotal",uint64_t(m.vTotal),32);
  setProperty("ReimsModeTimingSource","adl-p-transcoder-a-combo-pll-link-mn");
  return true;
 }
 void snapshotDisplayState(const char*key){
  // Read-only Intel/i915 plane, pipe, PPS and PWM registers. Capture before
  // boot FB retirement and after handoff to distinguish pixels from backlight.
  const uint32_t offsets[]={0x42084,0x70000,0x70008,0x70180,0x70188,0x70190,0x7019c,
   0x701ac,0x701cc,0x70240,0x7027c,0x44400,0x44408,
   0xc7200,0xc7204,0xc8250,0xc8254,0xc8258,0xc8350,0xc8354,0xc8358};
  uint32_t pairs[sizeof(offsets)/sizeof(offsets[0])*2];
  for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);++i){pairs[2*i]=offsets[i];pairs[2*i+1]=reg(offsets[i]);}
  auto*d=OSData::withBytes(pairs,sizeof(pairs));if(d){setProperty(key,d);d->release();}
 }
 // A retain alone cannot preserve GPU mapping preparation. Native resources
 // pair object-vptr+170 prepare with +178 complete (not MemoryMap slots!).
 bool pinResource(OSObject*r){
  const auto*b=reinterpret_cast<const uint8_t*>(r);
  uint32_t count=0;memcpy(&count,b+0x28,4);
  if(!count||!ReimsNativeMapping::classIs(r,"IGAccelResource"))return false;
  auto*v=*reinterpret_cast<const uintptr_t*const*>(r);
  if(!v[0x170/8]||!reinterpret_cast<bool(*)(OSObject*)>(v[0x170/8])(r))return false;
  r->retain();++pinAcquires;return true;
 }
 void unpinResource(OSObject*&r){
  if(!r)return;auto*v=*reinterpret_cast<const uintptr_t*const*>(r);
  reinterpret_cast<void(*)(OSObject*)>(v[0x178/8])(r);
  r->release();r=nullptr;++pinReleases;
 }
 void releaseFront(){unpinResource(frontResource);OSSafeReleaseNULL(frontSurface);}
 void publishFlip(bool force=false){
  if(optimizeScanout&&!force)return;
  setProperty("ReimsFlipSubmitted",flipState.submitted,64);
  setProperty("ReimsFlipLatched",flipState.latched,64);
  setProperty("ReimsFlipCompleted",flipState.completed,64);
  setProperty("ReimsFlipLiveBase",uint64_t(liveBase),32);
  setProperty("ReimsFlipPending",flipState.pending);
  setProperty("ReimsScanoutPinAcquires",pinAcquires,64);
  setProperty("ReimsScanoutPinReleases",pinReleases,64);
  setProperty("ReimsITCMCalls",itcmCalls,64);
  setProperty("ReimsHardwareWraps",wraps,64);
  setProperty("ReimsVBLCallbacks",vblCallbacks,64);
  setProperty("ReimsNativeVBLCallbacks",nativeVBLCallbacks,64);
 }
 void writeReg(uint32_t offset,uint32_t value){
  *reinterpret_cast<volatile uint32_t*>(registers->getVirtualAddress()+offset)=value;
  OSSynchronizeIO();
 }
 void observeScanout(){
  const uint32_t frame=reg(0x70040);
  // Hardware 32-bit counter preserves frames missed by a delayed poll. The
  // running mode/pipe is preserved; unsigned subtraction covers rollover.
  wraps+=uint32_t(frame-lastFrame);lastFrame=frame;
  liveBase=reg(0x701ac)&0xfffff000U;
  if(flipState.observe(liveBase,wraps)){if(auto*p=pendingProfile())p->latched=nowNS();publishFlip();}
 }
 bool finishFlip(){
  if(!flipState.finish())return false;
  if(auto*p=pendingProfile())p->completed=nowNS();
  // Called from native itcm under its accelerator gate, never from our timer.
  // The new live surface no longer scans the old mapping; release exactly once.
  releaseFront();frontResource=pendingResource;pendingResource=nullptr;
  frontSurface=pendingSurface;pendingSurface=nullptr;
  if(auto*p=pendingProfile())p->released=nowNS();profilePending=UINT32_MAX;
  publishFlip();return true;
 }
 IOReturn armPrimary(OSObject*r,IOSurface*s,const ReimsNativeLayout::Plan*plan,uint32_t va){
  if(flipState.pending)return kIOReturnBusy;
  if(r&&plan->planeTilingBits){
   // ADL-P display13 routes tiled surfaces through DPT unless disabled.
   // Native TGL maps GGTT pages, not a GGTT-resident display page table.
   // Linux intel_dpt_configure uses CHICKEN_MISC_2[30] for this exact choice.
   const uint32_t current=reg(0x42084),disableDPT=1U<<30;
   diag("ReimsDPTControlBefore",uint64_t(current),32);
   if(!(current&disableDPT)){
    // This is a global mode. Do not change interpretation for another active
    // tiled plane or for a preexisting tiled primary owned by another driver.
    for(unsigned pipe=0;pipe<4;++pipe)for(unsigned plane=0;plane<7;++plane){
     const uint32_t ctl=reg(0x70180+pipe*0x1000+plane*0x100);
     if((ctl&0x80000000U)&&(ctl&(7U<<10)))return kIOReturnUnsupported;
    }
    savedDPT=current;writeReg(0x42084,current|disableDPT);changedDPT=true;
    if(!(reg(0x42084)&disableDPT))return kIOReturnIOError;
   }
   diag("ReimsDPTControlAfter",uint64_t(reg(0x42084)),32);
  }
  observeScanout();
  // pinResource has already succeeded before reaching this function.
  if(!flipState.arm(va,r!=nullptr,wraps))return kIOReturnBusy;
  pendingResource=r;pendingSurface=s;if(s)s->retain();
  uint32_t ctl=bootPlaneCtl&~((7U<<10)|(1U<<9)|(1U<<8)|3U|(1U<<15)|(1U<<4));
  if(r){
   ctl|=0x80000000U|plan->planeTilingBits;
   writeReg(0x70188,plan->planeStride);writeReg(0x7018c,0);
   writeReg(0x70190,(1079U<<16)|1919U);writeReg(0x701a4,0);
  }else ctl&=~0x80000000U;
  // Intel MMIO sequence: nonarming fields, CTL, then SURF to arm atomically.
  writeReg(0x70180,ctl);writeReg(0x7019c,va);(void)reg(0x7019c);
  profilePending=profileCurrent;
  if(auto*p=currentProfile()){p->armed=nowNS();p->frame=lastFrame;p->base=va;}
  diag("ReimsDisplayPhase","native-primary-flip");publishFlip();
  return kIOReturnSuccess;
 }
 ReimsNativeLayout::Input surfaceLayout(OSObject*r,IOSurface*s){
  ReimsNativeLayout::Input i;
  if(!r||!r->metaCast("IGAccelResource")||!s)return i;
  auto*m=s->getMemoryDescriptor();if(!m)return i;
  const auto*b=reinterpret_cast<const uint8_t*>(r);
  i.width=s->getWidth();i.height=s->getHeight();i.surfacePitch=s->getPlaneBytesPerRow(0);
  i.fourcc=s->getPixelFormat();i.planeCount=s->getPlaneCount();
  i.clientOffset=s->getClientAlignedOffset();i.planeOffset=s->getPlaneOffset(0);i.backingLength=m->getLength();
  memcpy(&i.rowA,b+0xb8,8);memcpy(&i.rowB,b+0xc0,8);
  memcpy(&i.tileMode,b+0x194,4);memcpy(&i.resourceFormat,b+0x190,4);
  auto*storage=static_cast<IOAccelResource2*>(r)->getStorageResource();
  if(storage&&storage->metaCast("IGAccelResource")){
   i.storageKnown=true;
   const uint8_t flags=reinterpret_cast<const uint8_t*>(storage)[0x211];
   i.compressed=(flags&2)!=0;
   diag("ReimsStorageFlags",uint64_t(flags),32);
  }
  return i;
 }
 void recordLayout(const ReimsNativeLayout::Plan&p){
  diag("ReimsLayoutStatus",uint64_t(p.status),32);
  diag("ReimsLayoutStride",uint64_t(p.planeStride),32);
  diag("ReimsLayoutTilingBits",uint64_t(p.planeTilingBits),32);
  diag("ReimsLayoutStorageHeight",uint64_t(p.storageHeight),32);
  diag("ReimsLayoutRequiredBytes",p.requiredBytes,64);
  diag("ReimsLayoutBaseAlignment",uint64_t(p.baseAlignment),32);
 }
 struct Gate {IORecursiveLock*l;Gate(IORecursiveLock*p):l(p){IORecursiveLockLock(l);}~Gate(){IORecursiveLockUnlock(l);}};
 uint32_t reg(uint32_t offset) const {
  auto*p=reinterpret_cast<volatile uint32_t*>(registers->getVirtualAddress()+offset);
  uint32_t value=*p;OSSynchronizeIO();return value;
 }
 static void tick(OSObject *o,IOTimerEventSource *source){
  auto*s=OSDynamicCast(ReimsIntelADLFramebuffer,o);if(!s)return;
  OSArray*snapshot=nullptr;
  {
   Gate g(s->stateLock);if(!s->ready)return;
   s->observeScanout();
   // flip/itcm can also observe the counter. Keep notification progress
   // separate so those reads cannot consume an IOGraphics VBL callback.
   if(s->wraps!=s->notifiedWraps){
    s->notifiedWraps=s->wraps;
    if(s->vblRegistrations){snapshot=s->vblRegistrations;snapshot->retain();}
   }
   source->setTimeoutUS(1000);
  }
  // Do not enter IOGraphics/native notification while holding our state lock.
  if(snapshot){
   for(unsigned i=0;i<snapshot->getCount();++i){
    auto*r=static_cast<ReimsVBLRegistration*>(snapshot->getObject(i));
    bool dispatch=false;
    {Gate g(s->stateLock);dispatch=s->ready&&r->registered&&r->enabled;
     if(dispatch){++s->vblCallbacks;if(r->target->metaCast("IOAccelDisplayPipe"))++s->nativeVBLCallbacks;}}
    // unregister prevents later dispatches. A dispatch already claimed owns
    // the registration and target until it returns (native refcon is null).
    if(dispatch)r->proc(r->target,r->ref);
   }
   snapshot->release();
  }
 }

public:
 bool configure(IOService*a,IOFramebuffer*boot){
  if(!a||!boot||!init())return false;
  stateLock=IORecursiveLockAlloc();if(!stateLock)return false;
  auto*p=OSDynamicCast(IOPCIDevice,a->getProvider());
  if(!p||p->configRead16(kIOPCIConfigVendorID)!=0x8086||p->configRead16(kIOPCIConfigDeviceID)!=0x46a3||boot->getProvider()!=p)return false;
  IODisplayModeID m=0;IOIndex d=0;
  if(boot->getCurrentDisplayMode(&m,&d)||boot->getPixelInformation(m,d,kIOFBSystemAperture,&pixel)||
     boot->getInformationForDisplayMode(m,&modeInfo)||pixel.activeWidth!=1920||pixel.activeHeight!=1080||pixel.bytesPerRow!=7680||pixel.bitsPerPixel!=32)return false;
  timing.flags=kIODetailedTimingValid;
  if(boot->getTimingInfoForDisplayMode(m,&timing))bzero(&timing,sizeof(timing));
  aperture=boot->getApertureRange(kIOFBSystemAperture);
  registers=p->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0,kIOMapInhibitCache);
  if(!aperture||aperture->getLength()<uint64_t(pixel.bytesPerRow)*pixel.activeHeight||!registers||registers->getLength()<0x1000000)return false;
  // Exact observed boot primary: enabled, linear, full 1920x1080, origin zero.
  if(!(reg(0x70180)&0x80000000)||(reg(0x70180)&(7U<<10))||reg(0x70188)!=120||
     reg(0x7018c)||reg(0x70190)!=((1079U<<16)|1919)||reg(0x701a4))return false;
  bootPlaneCtl=reg(0x70180);
  snapshotDisplayState("ReimsBootDisplayStateV1");
  // Refuse the handoff before retiring boot FB if this is not a decoded
  // supported running mode. Never advertise a guessed 144Hz timing.
  const bool hardwareTimingOK=configureHardwareTiming();
  // Retain the read-only evidence on PCI even when refusing an unsupported
  // handoff; the temporary framebuffer then goes away but boot FB stays live.
  if(auto*raw=getProperty("ReimsDisplayTimingRegistersV1"))p->setProperty("ReimsDisplayTimingRegistersV1",raw);
  if(auto*status=getProperty("ReimsHardwareTimingStatus"))p->setProperty("ReimsHardwareTimingStatus",status);
  if(!hardwareTimingOK)return false;
  accelerator=a;a->retain();
  char path[1024];int length=sizeof(path);if(!a->getPath(path,&length,gIOServicePlane))return false;
  setProperty("IOAccelTypes",path);setProperty("IOAccelIndex",uint64_t(0),32);
  setProperty("ReimsDisplayPhase","native-framebuffer-platform-trace");
  setProperty("ReimsVBlankSource","pipe-a-hardware-frame-counter-poll");
  setProperty("ReimsPreservedBootSurface",uint64_t(reg(0x7019c)),32);
  return true;
 }
 bool start(IOService*p) override {return IOFramebuffer::start(p);}
 IOReturn enableController() override {
  if(ready)return kIOReturnSuccess;
  // IOFramebuffer's controller gate is held during WSAA / mode-change
  // notifications, whose native IOAccel handlers drain the transaction queue.
  // The queue needs a VBL to observe itcm. Running this timer on that same gate
  // deadlocks shutdown: drain waits for VBL, VBL waits for the controller gate.
  // Keep the base getWorkLoop() unchanged; only completion polling is separate.
  completionWorkLoop=IOWorkLoop::workLoop();
  if(!completionWorkLoop)return kIOReturnNoResources;
  timer=IOTimerEventSource::timerEventSource(this,tick);
  if(!timer||completionWorkLoop->addEventSource(timer)){
   OSSafeReleaseNULL(timer);OSSafeReleaseNULL(completionWorkLoop);return kIOReturnNoResources;
  }
  lastFrame=reg(0x70040);ready=true;timer->setTimeoutUS(1000);
  setProperty("ReimsCompletionWorkLoop","independent-hardware-vblank");
  // IOFramebuffer initializes/joins the PM tree but the concrete driver must
  // register its own states. Without an on output power character the child
  // AppleBacklightDisplay stays at state0 and drops brightness commits.
  // Like IOBootNDRV's doze-only policy, keep system sleep blocked until real
  // GPU/panel sleep-wake is supported. Idle transitions preserve pipe/mappings;
  // the AppleBacklightDisplay child controls PWM illumination separately.
  IOPMPowerState states[3]={
   {1,kIOPMPreventSystemSleep,0,0,0,0,0,0,0,0,0,0},
   {1,kIOPMPreventSystemSleep,0,IOPMPowerOn,0,0,0,0,0,0,0,0},
   {1,kIOPMDeviceUsable|kIOPMPreventSystemSleep,IOPMPowerOn,IOPMPowerOn,0,0,0,0,0,0,0,0}
  };
  registerPowerDriver(this,states,3);temporaryPowerClampOn();changePowerStateTo(1);
  setProperty("ReimsControllerReady",true);return kIOReturnSuccess;
 }
 unsigned long maxCapabilityForDomainState(IOPMPowerFlags f) override{return (f&IOPMPowerOn)?2:0;}
 unsigned long initialPowerStateForDomainState(IOPMPowerFlags f) override{return (f&IOPMPowerOn)?2:0;}
 unsigned long powerStateForDomainState(IOPMPowerFlags f) override{return (f&IOPMPowerOn)?getPowerState():0;}
 IOReturn setAttribute(IOSelect a,uintptr_t value) override {
  if(a!=kIOPowerAttribute&&a!=kIODriverPowerAttribute)return IOFramebuffer::setAttribute(a,value);
  if(value>2)return kIOReturnBadArgument;
  if(value==hardwarePowerState)return kIOReturnSuccess;
  if(value==2&&(!ready||!(reg(0x70008)&0x80000000U)))return kIOReturnNotReady;
  const bool wasOn=hardwarePowerState==2,isOn=value==2;
  if(wasOn!=isOn)IOFramebuffer::handleEvent(isOn?kIOFBNotifyWillPowerOn:kIOFBNotifyWillPowerOff);
  hardwarePowerState=uint32_t(value);
  setProperty("ReimsAcceptedDisplayPowerState",uint64_t(value),32);
  if(wasOn!=isOn)IOFramebuffer::handleEvent(isOn?kIOFBNotifyDidPowerOn:kIOFBNotifyDidPowerOff);
  return kIOReturnSuccess;
 }
 void stop(IOService*p) override {
  if(stateLock){Gate g(stateLock);ready=false;}
  if(frontResource||pendingResource){
   // Disable scanout before completing pins during explicit detach. A stopped
   // pipe also cannot fetch the old pages. On a hardware timeout keep pins.
   writeReg(0x70180,reg(0x70180)&~0x80000000U);writeReg(0x7019c,0);(void)reg(0x7019c);
   uint32_t line=reg(0x70000)&0x1fff;unsigned seen=0;
   for(unsigned i=0;i<100&&seen<2;++i){IOSleep(1);uint32_t next=reg(0x70000)&0x1fff;if(next<line)++seen;line=next;}
   if(seen>=2&&(reg(0x701ac)&0xfffff000U)==0){
    releaseFront();unpinResource(pendingResource);OSSafeReleaseNULL(pendingSurface);
    if(changedDPT){writeReg(0x42084,(reg(0x42084)&~(1U<<30))|(savedDPT&(1U<<30)));changedDPT=false;}
   }
   else setProperty("ReimsScanoutPinsHeldAtStop",true);
  }
  // No stateLock here: an in-progress action may be waiting to acquire it.
  if(timer){timer->cancelTimeout();completionWorkLoop->removeEventSource(timer);OSSafeReleaseNULL(timer);}
  OSSafeReleaseNULL(completionWorkLoop);
  clearProofs();
  IOFramebuffer::stop(p);
 }
 void free() override {
  if(profile){IOFree(profile,ReimsFlipProfile::capacity*sizeof(*profile));profile=nullptr;}
  OSSafeReleaseNULL(aperture);OSSafeReleaseNULL(registers);OSSafeReleaseNULL(accelerator);OSSafeReleaseNULL(vblRegistrations);
  if(stateLock)IORecursiveLockFree(stateLock);IOFramebuffer::free();
 }
 IOReturn setProperties(OSObject*properties) override{
  auto*d=OSDynamicCast(OSDictionary,properties);
  if(d&&(d->getObject("StartFlipProfile")==kOSBooleanTrue||d->getObject("StopFlipProfile")==kOSBooleanTrue)){
   if(IOUserClient::clientHasPrivilege(current_task(),kIOClientPrivilegeAdministrator)!=kIOReturnSuccess)return kIOReturnNotPrivileged;
   Gate g(stateLock);
   if(d->getObject("StartFlipProfile")==kOSBooleanTrue){
    if(d->getObject("OptimizeScanout")&&!OSDynamicCast(OSBoolean,d->getObject("OptimizeScanout")))return kIOReturnBadArgument;
    if(!profile)profile=static_cast<ReimsFlipProfile::Record*>(IOMalloc(ReimsFlipProfile::capacity*sizeof(*profile)));
    if(!profile)return kIOReturnNoMemory;
    bzero(profile,ReimsFlipProfile::capacity*sizeof(*profile));profileCount=profileDropped=0;
    profileCurrent=profilePending=UINT32_MAX;profileEnabled=true;
    if(auto*b=OSDynamicCast(OSBoolean,d->getObject("OptimizeScanout")))optimizeScanout=b->getValue();
   }else{profileEnabled=false;publishProfile();publishFlip(true);}
   return kIOReturnSuccess;
  }
  if(!d||(d->getObject("CaptureNativeFront")!=kOSBooleanTrue&&d->getObject("ClearNativeFront")!=kOSBooleanTrue&&d->getObject("CaptureDisplayTiming")!=kOSBooleanTrue))return IOFramebuffer::setProperties(properties);
  if(IOUserClient::clientHasPrivilege(current_task(),kIOClientPrivilegeAdministrator)!=kIOReturnSuccess)return kIOReturnNotPrivileged;
  if(d->getObject("CaptureDisplayTiming")==kOSBooleanTrue){Gate g(stateLock);captureTiming();publishFlip(true);publishProfile();return kIOReturnSuccess;}
  if(d->getObject("ClearNativeFront")==kOSBooleanTrue){Gate g(stateLock);removeProperty("ReimsFrontResidentBytes");removeProperty("ReimsFrontBackingBytes");return kIOReturnSuccess;}
  Gate g(stateLock);snapshotDisplayState("ReimsCurrentDisplayStateV1");
  if(!ready||flipState.pending||!frontResource||!frontSurface)return kIOReturnNotReady;
  auto*backing=frontSurface->getMemoryDescriptor();
  const auto plan=ReimsNativeLayout::describe(surfaceLayout(frontResource,frontSurface));
  if(plan.status!=ReimsNativeLayout::Status::OK)return kIOReturnUnsupported;
  const auto proof=ReimsNativeMapping::inspect(frontResource,backing,registers,flipState.base,plan.requiredBytes);
  if(proof.code!=ReimsNativeMapping::ResultCode::Success)return kIOReturnNotReady;
  // The verified map and its resident descriptor remain prepared by our front
  // resource pin. No MMIO or pixel writes and no private mapping modifications.
  auto*map=*reinterpret_cast<OSObject*const*>(reinterpret_cast<const uint8_t*>(frontResource)+0x40);
  auto*memory=*reinterpret_cast<OSObject*const*>(reinterpret_cast<const uint8_t*>(map)+0x18);
  auto*resident=*reinterpret_cast<IOMemoryDescriptor*const*>(reinterpret_cast<const uint8_t*>(memory)+0xd0);
  ReimsNativeMapping::Retained holdResident(resident);
  ReimsNativeMapping::Pinned holdBacking(backing);if(holdBacking.status)return holdBacking.status;
  const size_t length=plan.requiredBytes;
  void*bytes=IOMalloc(length);if(!bytes)return kIOReturnNoMemory;
  removeProperty("ReimsFrontResidentBytes");removeProperty("ReimsFrontBackingBytes");
  IOMemoryDescriptor*descriptors[]={resident,backing};
  const char*names[]={"ReimsFrontResidentBytes","ReimsFrontBackingBytes"};
  bool ok=true;
  for(unsigned i=0;i<2&&ok;++i){
   ok=descriptors[i]->readBytes(0,bytes,length)==length;
   if(ok){auto*data=OSData::withBytes(bytes,length);ok=data&&setProperty(names[i],data);OSSafeReleaseNULL(data);}
  }
  IOFree(bytes,length);
  setProperty("ReimsFrontCaptureStamp",flipState.completed,64);
  setProperty("ReimsFrontCaptureBase",uint64_t(flipState.base),32);
  snapshotDisplayState("ReimsCurrentDisplayStateV1");
  return ok?kIOReturnSuccess:kIOReturnIOError;
 }
 IODeviceMemory *getApertureRange(IOPixelAperture a) override {if(a!=kIOFBSystemAperture||!aperture)return nullptr;aperture->retain();return aperture;}
 // Accelerator discovery precedes enableController and base pixel setup.
 // Return the validated, retained boot aperture even during that early probe.
 IODeviceMemory *getVRAMRange() override {return getApertureRange(kIOFBSystemAperture);}
 const char*getPixelFormats() override{return IO32BitDirectPixels "\0";}
 UInt64 getPixelFormatsForDisplayMode(IODisplayModeID m,IOIndex d) override{return m==1&&!d?1:0;}
 IOItemCount getDisplayModeCount() override{return 1;}
 IOReturn getDisplayModes(IODisplayModeID*m) override{if(!m)return kIOReturnBadArgument;*m=1;return kIOReturnSuccess;}
 IOReturn getInformationForDisplayMode(IODisplayModeID m,IODisplayModeInformation*i) override {if(m!=1||!i)return kIOReturnBadArgument;*i=modeInfo;i->flags|=kDisplayModeValidFlag|kDisplayModeSafeFlag|kDisplayModeDefaultFlag;return kIOReturnSuccess;}
 IOReturn getPixelInformation(IODisplayModeID m,IOIndex d,IOPixelAperture a,IOPixelInformation*i) override{if(m!=1||d||a!=kIOFBSystemAperture||!i)return kIOReturnBadArgument;*i=pixel;return kIOReturnSuccess;}
 IOReturn getCurrentDisplayMode(IODisplayModeID*m,IOIndex*d) override{if(!m||!d)return kIOReturnBadArgument;*m=1;*d=0;return kIOReturnSuccess;}
 IOReturn setDisplayMode(IODisplayModeID m,IOIndex d) override{return m==1&&!d?kIOReturnSuccess:kIOReturnUnsupported;}
 IOReturn getStartupDisplayMode(IODisplayModeID*m,IOIndex*d) override{return getCurrentDisplayMode(m,d);}
 IOReturn setStartupDisplayMode(IODisplayModeID m,IOIndex d) override{return setDisplayMode(m,d);}
 IOReturn connectFlags(IOIndex c,IODisplayModeID m,IOOptionBits*f) override{if(c||m!=1||!f)return kIOReturnBadArgument;*f=kDisplayModeValidFlag|kDisplayModeSafeFlag|kDisplayModeDefaultFlag;return kIOReturnSuccess;}
 IOReturn getTimingInfoForDisplayMode(IODisplayModeID m,IOTimingInformation*i) override{if(m!=1||!i)return kIOReturnBadArgument;*i=timing;return kIOReturnSuccess;}
 IOItemCount getConnectionCount() override{return 1;}
 IOReturn getAttribute(IOSelect a,uintptr_t*v) override{if(a==kIOHardwareCursorAttribute){if(v)*v=0;return kIOReturnSuccess;}return IOFramebuffer::getAttribute(a,v);}
 IOReturn getAttributeForConnection(IOIndex c,IOSelect a,uintptr_t*v) override{
  if(c)return kIOReturnBadArgument;
  if(a==kConnectionEnable||a==kConnectionCheckEnable){if(v)*v=ready;return kIOReturnSuccess;}
  if(a==kConnectionFlags){if(v)*v=kIOConnectionBuiltIn;return kIOReturnSuccess;}
  if(a==kConnectionChanged){if(v)*v=0;return kIOReturnSuccess;}
  return IOFramebuffer::getAttributeForConnection(c,a,v);
 }
 IOReturn getAppleSense(IOIndex c,UInt32*s,UInt32*e,UInt32*t,UInt32*d) override{if(c)return kIOReturnBadArgument;if(s)*s=0;if(e)*e=0;if(t)*t=0;if(d)*d=kPanelTFTConnect;return kIOReturnSuccess;}
 IOReturn registerForInterruptType(IOSelect type,IOFBInterruptProc proc,OSObject*target,void*ref,void**out) override{
  Gate g(stateLock);if(type!=kIOFBVBLInterruptType)return kIOReturnUnsupported;
  if(!out||!proc||!target)return kIOReturnBadArgument;*out=nullptr;
  auto*r=new ReimsVBLRegistration;
  if(!r)return kIOReturnNoMemory;
  if(!r->init()){r->release();return kIOReturnNoMemory;}
  r->proc=proc;r->target=target;target->retain();r->ref=ref;
  auto*next=vblRegistrations?OSArray::withArray(vblRegistrations):OSArray::withCapacity(2);
  if(!next||!next->setObject(r)){OSSafeReleaseNULL(next);r->release();return kIOReturnNoMemory;}
  OSSafeReleaseNULL(vblRegistrations);vblRegistrations=next;*out=r;r->release();
  setProperty("ReimsVBLCallbackClass",target->getMetaClass()->getClassName());
  setProperty("ReimsVBLSubscribers",uint64_t(next->getCount()),32);
  IOLog("ReimsIntelADLFramebuffer VBL registered target=%s\n",target->getMetaClass()->getClassName());
  return kIOReturnSuccess;
 }
 IOReturn unregisterInterrupt(void*ref) override{
  Gate g(stateLock);if(!vblRegistrations)return kIOReturnBadArgument;
  for(unsigned i=0;i<vblRegistrations->getCount();++i){
   auto*r=static_cast<ReimsVBLRegistration*>(vblRegistrations->getObject(i));if(r!=ref)continue;
   // Mark inactive first: even an older retained snapshot cannot dispatch it.
   r->enabled=false;r->registered=false;
   auto*next=OSArray::withArray(vblRegistrations);
   if(next){next->removeObject(i);OSSafeReleaseNULL(vblRegistrations);vblRegistrations=next;}
   // On allocation failure keep the inactive registration until teardown;
   // never reactivate or invoke it merely to save its bookkeeping allocation.
   return kIOReturnSuccess;
  }
  return kIOReturnBadArgument;
 }
 IOReturn setInterruptState(void*ref,UInt32 state) override{
  Gate g(stateLock);if(!vblRegistrations)return kIOReturnBadArgument;
  for(unsigned i=0;i<vblRegistrations->getCount();++i){
   auto*r=static_cast<ReimsVBLRegistration*>(vblRegistrations->getObject(i));
   if(r==ref&&r->registered){r->enabled=state==kEnabledInterruptState;return kIOReturnSuccess;}
  }
  return kIOReturnBadArgument;
 }
 IOReturn callPlatformFunction(const OSSymbol*name,bool wait,void*p1,void*p2,void*p3,void*p4) override{
  // These are the actual OSSymbol strings initialized by native TGL 16.0.0.
  // fSymFBCLASS* are C++ variable names, never runtime platform function names.
  // Falling through with wait=true sends an unimplemented request to the PCI
  // provider and IOPlatformExpert then waits indefinitely for IOResources.
  static const char*const functions[]={"ra","ura","rvh","gsri","r32","w32",
   "vald","flip","itcm","pftn","gama","psrx","csc","pgam",
   "fRC6HandlingFuncs","fInterruptCallbacks","gdwv"};
  bool native=false;
  if(name)for(auto function:functions)if(name->isEqualTo(function)){native=true;break;}
  if(native){
   const uint64_t requestNS=nowNS();
   Gate g(stateLock);if(platformCalls++<32){IOLog("ReimsIntelFB platform %s\n",name->getCStringNoCopy());diag("ReimsLastPlatformCall",name->getCStringNoCopy());}
   if(name->isEqualTo("vald")){
    diag("ReimsValidationStage",uint64_t(1),32);
    if(!p1||!p2||p3||p4)return kIOReturnBadArgument;
    diag("ReimsValidationStage",uint64_t(2),32);
    auto*object=static_cast<OSObject*>(p1);
    diag("ReimsTransactionClass",object->getMetaClass()->getClassName());
    if(!object->metaCast("IOAccelDisplayPipeTransaction2"))return kIOReturnBadArgument;
    const uint64_t kind=*static_cast<uint64_t*>(p2);
    diag("ReimsValidationStage",uint64_t(3),32);
    diag("ReimsValidationKind",kind,64);
    if(kind!=1&&kind!=2)return kIOReturnUnsupported;
    auto*t=static_cast<IOAccelDisplayPipeTransaction2*>(object);
    const unsigned plane=kind==2?1:0;
    auto*r=t->getPlaneResource(plane,0);auto*s=t->getPlaneIOSurface(plane,0);
    diag("ReimsValidationPlane",uint64_t(plane),32);
    diag("ReimsValidationStage",uint64_t(4),32);
    diag("ReimsPrimaryPresence",uint64_t((r?1:0)|(s?2:0)),32);
    if(r)diag("ReimsResourceClass",r->getMetaClass()->getClassName());
    // Native performTransaction explicitly permits empty/disable requests.
    // They are valid input; the flip implementation must handle or reject them.
    if(!r||!s)return kIOReturnSuccess;
    if(!r->metaCast("IGAccelResource"))return kIOReturnUnsupported;
    diag("ReimsValidationStage",uint64_t(5),32);
    auto*m=s->getMemoryDescriptor();if(!m)return kIOReturnUnsupported;
    diag("ReimsValidationStage",uint64_t(6),32);
    // Native TGL resource row-byte candidates and layout. These are NOT
    // GPU addresses; the actual flip record stores its mapping VA at +8.
    uint64_t rowA=0,rowB=0;uint32_t layout=0,format=0,pitch=0;
    const auto*bytes=reinterpret_cast<const uint8_t*>(r);
    memcpy(&rowA,bytes+0xb8,8);memcpy(&rowB,bytes+0xc0,8);
    memcpy(&layout,bytes+0x194,4);memcpy(&format,bytes+0x190,4);memcpy(&pitch,bytes+0x188,4);
    uint64_t info[]={1,kind,t->getTransactionDirtyBits(),s->getWidth(),s->getHeight(),
     s->getPlaneBytesPerRow(0),s->getPixelFormat(),s->getPlaneCount(),
     s->getClientAlignedOffset(),s->getPlaneOffset(0),m->getLength(),
     rowA,rowB,layout,format,pitch};
    if(validations++<32){auto*d=OSData::withBytes(info,sizeof(info));if(d){diag("ReimsNativeSurfaceV1",d);d->release();}
     IOLog("ReimsIntelFB validate kind=%llu dirty=0x%llx size=%llux%llu row=%llu format=0x%llx layout=%u rows=%llu/%llu\n",
      kind,info[2],info[3],info[4],info[5],info[6],layout,rowA,rowB);}
    const auto plan=ReimsNativeLayout::describe(surfaceLayout(r,s));
    recordLayout(plan);
    if(plan.status!=ReimsNativeLayout::Status::OK)return kIOReturnUnsupported;
    diag("ReimsValidationStage",uint64_t(7),32);
    return kIOReturnSuccess;
   }
   if(name->isEqualTo("flip")&&p1&&p2&&p3&&!p4){
    ReimsFlipProfile::Record*trace=nullptr;
    profileCurrent=UINT32_MAX;
    if(profileEnabled&&profile){
     if(profileCount<ReimsFlipProfile::capacity){profileCurrent=profileCount++;trace=&profile[profileCurrent];
      trace->request=requestNS;trace->locked=nowNS();}
     else ++profileDropped;
    }
    struct FinishTrace {ReimsIntelADLFramebuffer*f;ReimsFlipProfile::Record*p;
     ~FinishTrace(){if(p)p->returned=nowNS();f->profileCurrent=UINT32_MAX;}} traceScope{this,trace};
    // Snapshot the native 16.0.0 stack record before it expires. No guessed
    // addresses are programmed and unsupported never fabricates completion.
    if(!optimizeScanout){auto*d=OSData::withBytes(p3,0xb8);if(d){diag("ReimsNativeFlipParameters",d);d->release();}}
    diag("ReimsNativeFlipKind",*static_cast<uint64_t*>(p2),64);
    diag("ReimsNativeFlipAttempts",++flips,64);
    auto*object=static_cast<OSObject*>(p1);
    if(!object->metaCast("IOAccelDisplayPipeTransaction2"))return kIOReturnBadArgument;
    const uint64_t kind=*static_cast<uint64_t*>(p2);
    // This milestone programs primary only; a second nonempty logical plane
    // requires its own format/blend/placement contract.
    if(kind!=1)return kIOReturnUnsupported;
    if(flipState.pending)return kIOReturnBusy;
    auto*t=static_cast<IOAccelDisplayPipeTransaction2*>(object);
    const unsigned plane=*static_cast<uint64_t*>(p2)==2?1:0;
    auto*r=t->getPlaneResource(plane,0);auto*s=t->getPlaneIOSurface(plane,0);
    if(!r&&!s){
     if(*static_cast<const uint8_t*>(p3))return kIOReturnBadArgument;
     return armPrimary(nullptr,nullptr,nullptr,0);
    }
    if(r&&s){
     if(trace)trace->surfaceID=s->getSurfaceID();
     const auto plan=ReimsNativeLayout::describe(surfaceLayout(r,s));
     recordLayout(plan);
     if(plan.status!=ReimsNativeLayout::Status::OK)return kIOReturnUnsupported;
     const auto*record=static_cast<const uint8_t*>(p3);
     uint32_t row=0,fourcc=0,selector=0;
     memcpy(&row,record+0x10,4);memcpy(&fourcc,record+0x3c,4);memcpy(&selector,record+0x40,4);
     // TGL 16.0 performTransaction+0x81361 reads IOSurface+0xa4 inline,
     // overwriting its default BGRA with zero on 25G83. This legacy copy is
     // not the current surface format. describe() above validates the real
     // exported getter AND resource format; never edit the IOSurface object.
     // Only tolerate an absent copy. A conflicting nonzero format, planar
     // surface, compression, selector or bad pitch must still be rejected.
     const uint32_t surfaceFourCC=s->getPixelFormat();
     const bool formatAgrees=surfaceFourCC==0x42475241&&(!fourcc||fourcc==surfaceFourCC);
     diag("ReimsFlipRecordFourCC",uint64_t(fourcc),32);
     diag("ReimsFlipSurfaceFourCC",uint64_t(surfaceFourCC),32);
     diag("ReimsLegacyFormatCopyIgnored",!fourcc&&formatAgrees);
     const bool consistent=record[0]==1&&row==plan.pitchBytes&&formatAgrees&&selector==0&&record[0x3a]==0;
     diag("ReimsFlipLayoutConsistent",consistent);
     if(!consistent)return kIOReturnUnsupported;
     const uint32_t fullRect[4]={0,0,0x44f00000U,0x44870000U}; // 0,0,1920,1080 float bits
     if(memcmp(record+0x14,fullRect,sizeof(fullRect))||memcmp(record+0x24,fullRect,sizeof(fullRect)))return kIOReturnUnsupported;
     uint64_t va=0;memcpy(&va,static_cast<const uint8_t*>(p3)+8,8);
     diag("ReimsLayoutBaseSuitable",ReimsNativeLayout::baseSuitable(plan,va));
     if(!ReimsNativeLayout::baseSuitable(plan,va))return kIOReturnUnsupported;
     // Native event_interrupt_gated has passed this transaction's write-stamp
     // barrier before TGL performTransaction calls flip. Keep its map prepared
     // independently until a later surface is confirmed live.
     if(trace)trace->layout=nowNS();
     if(!pinResource(r))return kIOReturnNotReady;
     if(trace)trace->pinned=nowNS();
     auto*preparedProof=proofFor(r,s);
     auto result=ReimsNativeMapping::inspect(r,s->getMemoryDescriptor(),registers,va,
       plan.requiredBytes,preparedProof);
     lastMapping=result;lastLayout=plan;
     if(result.cachedProof)++proofHits;else ++proofMisses;
     if(trace){trace->mapped=nowNS();trace->cached=result.cachedProof;}
     forceDiagnostics=result.code!=ReimsNativeMapping::ResultCode::Success;
     diag("ReimsMappingAliasMismatchPage",result.aliasMismatchPage,64);
     diag("ReimsMappingBackingRelation",uint64_t(result.backingRelation),32);
     diag("ReimsMappingSurfaceBackingIdentity",result.surfaceBackingIdentity);
     diag("ReimsMappingUsesResident",result.nativeUsesResident);
     diag("ReimsMappingStorageDescriptorLength",result.storageDescriptorLength,64);
     diag("ReimsMappingResult",uint64_t(result.code),32);
     diag("ReimsMappingPagesChecked",uint64_t(result.pagesChecked),32);
     diag("ReimsMappingNativeVA",result.nativeGPUVA,64);
     diag("ReimsMappingFirstPTE",result.firstPTE,64);
     diag("ReimsMappingFirstPhysical",result.firstDescriptorPhysical,64);
     diag("ReimsMappingNativeFirstPhysical",result.nativeDescriptorFirstPhysical,64);
     diag("ReimsMappingAliasPagesChecked",uint64_t(result.aliasPagesChecked),32);
     diag("ReimsMappingPhysicalAlias",result.physicalAlias);
     diag("ReimsMappingDescriptorIdentity",result.descriptorIdentity);
     diag("ReimsMappingNativeDescriptorLength",result.nativeDescriptorLength,64);
     diag("ReimsMappingSurfaceDescriptorLength",result.surfaceDescriptorLength,64);
     diag("ReimsMappingResourceOffset",result.resourceBackingOffset,64);
     diag("ReimsMappingLength",result.nativeLength,64);
     diag("ReimsMappingAssignedLength",result.assignedLength,64);
     diag("ReimsMappingPrepareStatus",uint64_t(result.prepareStatus),32);
     diag("ReimsMappingFlags",uint64_t(result.mapFlags),32);
     diag("ReimsMappingMemoryFlags",uint64_t(result.memoryFlags),32);
     diag("ReimsMappingResourceType",uint64_t(result.resourceType),32);
     diag("ReimsMappingMismatchPage",result.mismatchPage,64);
     if(result.mapClass)diag("ReimsMappingClass",result.mapClass);
     if(result.ownerClass)diag("ReimsMappingOwnerClass",result.ownerClass);
     if(result.memoryClass)diag("ReimsMappingMemoryClass",result.memoryClass);
     if(result.nativeDescriptorClass)diag("ReimsMappingNativeDescriptorClass",result.nativeDescriptorClass);
     if(result.surfaceDescriptorClass)diag("ReimsMappingSurfaceDescriptorClass",result.surfaceDescriptorClass);
     forceDiagnostics=false;
     if(trace)trace->diagnostics=nowNS();
     if(result.code!=ReimsNativeMapping::ResultCode::Success){unpinResource(r);return kIOReturnUnsupported;}
     const auto rc=armPrimary(r,s,&plan,uint32_t(va));
     if(rc!=kIOReturnSuccess)unpinResource(r);
     return rc;
    }
   }
   if(name->isEqualTo("itcm")&&p1&&!p2&&!p3&&!p4){
    ++itcmCalls;observeScanout();finishFlip();
    *static_cast<uint32_t*>(p1)=flipState.complete()?UINT32_MAX:0;
    return kIOReturnSuccess;
   }
   return kIOReturnUnsupported;
  }
  return IOFramebuffer::callPlatformFunction(name,wait,p1,p2,p3,p4);
 }
};
OSDefineMetaClassAndStructors(ReimsIntelADLFramebuffer,IOFramebuffer)
IOFramebuffer*ReimsIntelFramebufferCreate(IOService*a,IOFramebuffer*boot){
 auto*f=new ReimsIntelADLFramebuffer;if(!f)return nullptr;
 if(!f->configure(a,boot)){f->release();return nullptr;}
 IOService*p=a->getProvider();
 // Ventura IOGraphics 597 message 0xe0016001 delivers the real terminated
 // notification under both IOGraphics gates. Synchronous stop disables these
 // notifiers, so it must not run first (IOAccelDisplayPipe borrows its FB).
 const IOReturn notified=boot->IOFramebuffer::message(0xe0016001,p,nullptr);
 IOLog("ReimsIntelFB boot retirement notification=0x%x id=0x%llx\n",notified,boot->getRegistryEntryID());
 if(notified!=kIOReturnSuccess){f->release();return nullptr;}
 if(!boot->terminate(kIOServiceRequired|kIOServiceSynchronous)){
  f->release();return nullptr;
 }
 if(!f->attach(p)){f->release();return nullptr;}
 if(!f->start(p)){f->detach(p);f->release();return nullptr;}
 f->registerService();return f;
}
