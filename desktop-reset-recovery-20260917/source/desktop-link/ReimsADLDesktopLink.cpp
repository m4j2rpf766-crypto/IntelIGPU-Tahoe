#include <IOKit/IOService.h>
#include "../backlight/parameters.inc"
#include <IOKit/IOUserClient.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <kern/clock.h>
#include "graphics_control.hpp"
#include "intel_framebuffer.hpp"
#include "reset_registry.hpp"

// Native TGL's exported, locked/refcounted forcewake API. The bundle explicitly
// depends on that driver; never compete for its hardware request bit directly.
extern "C" void ReimsNativeSafeForceWake(IOService*,bool,unsigned)
 __asm__("__ZN16IntelAccelerator13SafeForceWakeEbj");
namespace {
constexpr uint32_t kIntelVendor = 0x8086;
constexpr uint32_t kADLP46A3 = 0x46a3;
constexpr uint32_t kRcsBase = 0x02000;
constexpr uint32_t kBcsBase = 0x22000;
constexpr uint32_t kRingPSMI = 0x50;
constexpr uint32_t kRingTail = kBcsBase + 0x30;
constexpr uint32_t kRingHead = kBcsBase + 0x34;
constexpr uint32_t kRingStart = kBcsBase + 0x38;
constexpr uint32_t kRingControl = kBcsBase + 0x3c;
constexpr uint32_t kRingHWS = kBcsBase + 0x80;
constexpr uint32_t kRingIPEHR = kBcsBase + 0x68;
constexpr uint32_t kRingACTHD = kBcsBase + 0x74;
constexpr uint32_t kRingPDP0Lo = kBcsBase + 0x270;
constexpr uint32_t kRingPDP0Hi = kBcsBase + 0x274;
constexpr uint32_t kForceWakeGTAck = 0x130044;
constexpr uint32_t kForceWakeRenderAck = 0x000d84;
// Pinned TGL SafeForceWake passes requester index 1 at 0x28dab; the
// Multithreaded implementation shifts 1 by that index (0x2d5ce).
// Thus its RENDER/GT acknowledgement is bit 1, not the old direct bit 0.
constexpr uint32_t kForceWakeBit = 1U << 1U;
constexpr uint32_t kADLPRequiredRCSPSMIBits = (1U << 12U) | (1U << 7U);

class NativeForceWake {
 IOService *accelerator;unsigned domains;
public:
 NativeForceWake(IOService *a,unsigned d):accelerator(a),domains(d){
  ReimsNativeSafeForceWake(accelerator,true,domains);
 }
 void release(){if(accelerator){ReimsNativeSafeForceWake(accelerator,false,domains);accelerator=nullptr;}}
 ~NativeForceWake(){release();}
 NativeForceWake(const NativeForceWake&)=delete;
 NativeForceWake& operator=(const NativeForceWake&)=delete;
};

bool read32(IOMemoryMap *map, uint32_t offset, uint32_t &value) {
 if(!map || offset > map->getLength() || map->getLength()-offset < sizeof(value))return false;
 auto *base=reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress());
 value=*reinterpret_cast<volatile uint32_t *>(base+offset);OSSynchronizeIO();return true;
}
bool write32(IOMemoryMap *map,uint32_t offset,uint32_t value){
 if(!map || offset > map->getLength() || map->getLength()-offset < sizeof(value))return false;
 auto *base=reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress());
 *reinterpret_cast<volatile uint32_t *>(base+offset)=value;OSSynchronizeIO();return true;
}
bool waitAck(IOMemoryMap *map,uint32_t offset,bool set,uint32_t &value){
 for(unsigned elapsed=0;elapsed<=50000;elapsed+=10){
  if(!read32(map,offset,value))return false;
  if(((value&kForceWakeBit)!=0)==set)return true;
  IODelay(10);
 }
 return false;
}
void setReg(IOService *service,const char *name,uint32_t value){service->setProperty(name,static_cast<uint64_t>(value),64);}

// Diagnostic-only CPU reads on this 16 GiB host. Restrict to high RAM (above
// the 32-bit PCI hole, below installed RAM plus that hole); never touch BAR2
// or modify GGTT PTEs to expose a page. No caller-supplied addresses are used.
bool copyHighRAM(uint64_t physical,void *output,size_t length){
 if(physical<0x100000000ULL||physical>=0x500000000ULL||
    !length||length>4096||(physical&4095)+length>4096)return false;
 auto *memory=IOMemoryDescriptor::withPhysicalAddress(physical,length,kIODirectionIn);
 if(!memory)return false;
 auto *mapping=memory->map(kIOMapAnywhere|kIOMapReadOnly);
 if(!mapping){memory->release();return false;}
 memcpy(output,reinterpret_cast<const void *>(mapping->getVirtualAddress()),length);
 mapping->release();memory->release();return true;
}

#include "command_capture.hpp"

struct BCSSnapshotV1 {
 uint32_t version,action,result,forceWakeBefore,forceWakeAfterAcquire;
 uint32_t headBefore,tailBefore,startBefore,controlBefore,hwsBefore;
 uint32_t pdp0LoBefore,pdp0HiBefore,ipehrBefore,acthdBefore;
 uint32_t headAfter,tailAfter,startAfter,controlAfter,hwsAfter;
 uint32_t pdp0LoAfter,pdp0HiAfter,ipehrAfter,acthdAfter;
};

struct RCSSnapshotV1 {
 uint32_t version,result,gtAckBefore,gtAckAfterAcquire;
 uint32_t renderAckBefore,renderAckAfterAcquire;
 uint32_t psmiBefore,unknown2054Before,headBefore,tailBefore,startBefore;
 uint32_t controlBefore,hwsBefore,pdp0LoBefore,pdp0HiBefore,ipehrBefore;
 uint32_t acthdBefore,execlistStatusBefore,contextStatusPointerBefore;
 uint32_t ccidBefore,resetControlBefore,miModeBefore,instpmBefore;
 uint32_t reg2140Before,reg4030Before,reg4040Before,reg4044Before;
 uint32_t reg4050Before,regCEC4Before;
 uint32_t psmiAfter,headAfter,tailAfter,ipehrAfter,acthdAfter;
 uint32_t gtAckAfterRelease,renderAckAfterRelease;
};
}

class ReimsADLDesktopLink : public IOService {
 OSDeclareDefaultStructors(ReimsADLDesktopLink)
 IOService *framebuffer=nullptr;
 ReimsADLGraphicsControl *graphicsControl=nullptr;
 OSObject *oldTypes=nullptr,*oldIndex=nullptr;
 OSObject *oldPipeCapabilities=nullptr;
 bool pipeCapabilitiesPublished=false;
 IOLock *lock=nullptr;
 // This is deliberately a live cache, not an "ever prepared" latch.  A GT
 // reset restores PSMI and invalidates the state that authorises a desktop
 // route, so every enable request refreshes it from hardware.
 bool rcsWorkaroundLive=false;
 IOReturn prepareResetRegistry(){
  // Caller holds this service lock and has proven the native RCS was never
  // started. Never mutate this vector from a timer or on a live desktop.
  auto &v=rcsResetRegisters;
  if(!reimsResetProfile(v))return kIOReturnUnsupported;
  if(v.count==33){setProperty("RCSResetPSMIRegistered",true);return kIOReturnSuccess;}
  constexpr size_t n=33;
  auto *entries=static_cast<ReimsResetEntry*>(IOMalloc(n*sizeof(ReimsResetEntry)));
  if(!entries)return kIOReturnNoMemory;
  bzero(entries,n*sizeof(ReimsResetEntry));
  memcpy(entries,v.entries,32*sizeof(ReimsResetEntry));
  entries[32].reg=0x2050;entries[32].masked=1;
  memcpy(entries[32].name,"ADLP_RCS_PSMI",14);
  auto *old=v.entries;const size_t oldBytes=v.capacity*sizeof(ReimsResetEntry);
  v.entries=entries;v.capacity=n;v.count=n;
  IOFree(old,oldBytes);
  // No pointer into this kext is retained. Native IGVector destructor owns it.
  setProperty("RCSResetPSMIRegistered",true);
  setReg(this,"RCSResetRegisterCount",static_cast<uint32_t>(v.count));
  return kIOReturnSuccess;
 }
 IOReturn captureRPS(){
  removeProperty("RPSSnapshotV1");
  auto *accel=getProvider();
  auto *pci=accel?OSDynamicCast(IOPCIDevice,accel->getProvider()):nullptr;
  if(!accel||accel->isInactive()||!pci||
     pci->configRead16(kIOPCIConfigVendorID)!=kIntelVendor||
     pci->configRead16(kIOPCIConfigDeviceID)!=kADLP46A3)return kIOReturnNotReady;
  auto *map=pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0,kIOMapInhibitCache);
  if(!map)return kIOReturnNoMemory;
  // Gen12/ADLP read-only, fixed registers. Keep native forcewake ownership;
  // do not write frequency requests, limit reasons or RC6 control registers.
  // State/actual below are sampled AFTER forcewake, not idle-state evidence.
  uint64_t values[12]={1},absolute=0;
  clock_get_uptime(&absolute);absolutetime_to_nanoseconds(absolute,&values[1]);
  NativeForceWake wake(accel,5);
  uint32_t gt=0,render=0;
  bool ok=waitAck(map,kForceWakeGTAck,true,gt)&&waitAck(map,kForceWakeRenderAck,true,render);
  constexpr uint32_t offsets[]={0x145998,0xa008,0x145948,0x1381b4,0x1381a8,0x138108,0x138060};
  for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0])&&ok;++i){
   uint32_t raw=0;ok=read32(map,offsets[i],raw);values[i+3]=raw;
  }
  wake.release();map->release();
  clock_get_uptime(&absolute);absolutetime_to_nanoseconds(absolute,&values[2]);
  values[10]=gt;values[11]=render;
  if(!ok)return kIOReturnNotReady;
  auto *data=OSData::withBytes(values,sizeof(values));
  if(!data)return kIOReturnNoMemory;
  setProperty("RPSSnapshotV1",data);data->release();return kIOReturnSuccess;
 }
 IOReturn captureRCSBatch(){
  removeProperty("RCSCommandEvidenceV2");
  removeProperty("RCSBatchPage");removeProperty("RCSBatchPageAfter");
  removeProperty("RCSBatchPostSyncPage");removeProperty("RCSBatchPostSyncPTEs");
  removeProperty("RCSBatchPostSyncVA");removeProperty("RCSBatchPostSyncPhysical");
  setProperty("RCSBatchStable",false);setReg(this,"RCSBatchResult",1);
  // A physical walk is invalid for an IOMMU IOVA. Refuse an active mapper.
  auto *match=serviceMatching("IOMapper");
  if(!match)return kIOReturnNoMemory;
  auto *mapper=copyMatchingService(match);match->release();
  if(mapper){mapper->release();setReg(this,"RCSBatchResult",2);return kIOReturnUnsupported;}
  if(inspectRCS(false,true)!=kIOReturnSuccess)return kIOReturnNotReady;
  auto *value=OSDynamicCast(OSData,copyProperty("RCSSnapshotV1"));
  if(!value)return kIOReturnNotReady;
  RCSSnapshotV1 snapshot={};
  const bool sized=value->getLength()==sizeof(snapshot);
  if(sized)memcpy(&snapshot,value->getBytesNoCopy(),sizeof(snapshot));
  value->release();if(!sized)return kIOReturnBadArgument;
  auto *pci=OSDynamicCast(IOPCIDevice,getProvider()->getProvider());
  auto *map=pci?pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0,kIOMapInhibitCache):nullptr;
  if(!map)return kIOReturnNoMemory;
  NativeForceWake wake(getProvider(),5); // native domain 1=render, 4=GT
  uint32_t gtAck=0,renderAck=0;
  if(!waitAck(map,kForceWakeGTAck,true,gtAck)||!waitAck(map,kForceWakeRenderAck,true,renderAck)){
   map->release();return kIOReturnNotReady;
  }
  ReimsCommandCapture::capture(this,map);
  if(!snapshot.acthdAfter||(snapshot.resetControlBefore&3U)){
   map->release();setReg(this,"RCSBatchResult",3);return kIOReturnNotReady;
  }
  uint32_t mode=0,rootLo=0,rootHi=0,acthd=0,reset=~0U;
  bool ok=read32(map,kRcsBase+0x29c,mode)&&read32(map,kRcsBase+0x270,rootLo)&&
   read32(map,kRcsBase+0x274,rootHi)&&read32(map,kRcsBase+0x74,acthd)&&
   read32(map,kRcsBase+0xd0,reset);
  const uint64_t root=(static_cast<uint64_t>(rootHi)<<32)|rootLo;
  setReg(this,"RCSBatchMode",mode);setReg(this,"RCSBatchACTHD",acthd);
  setProperty("RCSBatchRoot",root,64);
  uint32_t descriptors[16]={};
  bool descriptorsOK=true;
  for(unsigned i=0;i<16;i++)descriptorsOK=read32(map,kRcsBase+0x510+i*4,descriptors[i])&&descriptorsOK;
  if(descriptorsOK){auto *data=OSData::withBytes(descriptors,sizeof(descriptors));
   if(data){setProperty("RCSBatchExeclistSQ",data);data->release();}}
  // RING_MODE's legacy PPGTT bit is zero with the native execlists path.
  // Keep the register as evidence; this diagnostic interprets PDP0 using the
  // exact runtime profile's 64-bit address-space configuration. A successful
  // walk is still only a sampled translation, not proof of GPU TLB contents.
  if(!ok||acthd!=snapshot.acthdAfter||(reset&3U)||
     root!=((static_cast<uint64_t>(snapshot.pdp0HiBefore)<<32)|snapshot.pdp0LoBefore)){
   map->release();setReg(this,"RCSBatchResult",4);return kIOReturnNotReady;
  }
  uint64_t entries[4]={},entryAddresses[4]={};
  uint64_t table=root&0x0000fffffffff000ULL;
  const unsigned shifts[4]={39,30,21,12};
  for(unsigned level=0;level<4;level++){
   entryAddresses[level]=table+((static_cast<uint64_t>(acthd)>>shifts[level])&511)*8;
   if(!copyHighRAM(entryAddresses[level],&entries[level],8)||!(entries[level]&1)||
      (level<3&&(entries[level]&0x80))){ok=false;break;}
   table=entries[level]&0x0000fffffffff000ULL;
  }
  auto *walk=OSData::withBytes(entries,sizeof(entries));
  if(walk){setProperty("RCSBatchPTEs",walk);walk->release();}
  auto *addresses=OSData::withBytes(entryAddresses,sizeof(entryAddresses));
  if(addresses){setProperty("RCSBatchPTEAddresses",addresses);addresses->release();}
  setProperty("RCSBatchPhysical",table,64);
  // Heap storage keeps the kernel stack small. Read twice, without claiming
  // that sequential snapshots constitute an atomic GPU/CPU synchronization.
  auto *pages=static_cast<uint8_t *>(IOMalloc(8192));
  if(!pages){map->release();return kIOReturnNoMemory;}
  if(ok)ok=copyHighRAM(table,pages,4096);
  bool stable=ok;
  for(unsigned level=0;stable&&level<4;level++){
   uint64_t entry=0;stable=copyHighRAM(entryAddresses[level],&entry,8)&&entry==entries[level];
  }
  if(ok){
   auto *page=OSData::withBytes(pages,4096);
   if(page){setProperty("RCSBatchPage",page);page->release();}else ok=false;
   if(copyHighRAM(table,pages+4096,4096)){
    auto *after=OSData::withBytes(pages+4096,4096);
    if(after){setProperty("RCSBatchPageAfter",after);after->release();}
    stable=stable&&!memcmp(pages,pages+4096,4096);
   }else stable=false;
  }
  // Decode only the captured PIPE_CONTROL immediately before ACTHD. Read its
  // PPGTT post-sync destination through the same root; do not alter any page.
  // This diagnoses the CPU-visible mapping, not GPU cache/TLB visibility.
  if(ok&&(acthd&4095)>=24){
   uint32_t pc[6]={};memcpy(pc,pages+(acthd&4095)-24,sizeof(pc));
   if(pc[0]==0x7a000004&&(pc[1]&(3U<<14))&&!(pc[1]&(1U<<24))){
    const uint64_t target=(static_cast<uint64_t>(pc[3])<<32)|pc[2];
    uint64_t targetTable=root&0x0000fffffffff000ULL,targetEntries[4]={};
    bool targetOK=!(target>>48);
    for(unsigned level=0;targetOK&&level<4;level++){
     const uint64_t address=targetTable+((target>>shifts[level])&511)*8;
     targetOK=copyHighRAM(address,&targetEntries[level],8)&&(targetEntries[level]&1)&&
      !(level<3&&(targetEntries[level]&0x80));
     targetTable=targetEntries[level]&0x0000fffffffff000ULL;
    }
    setProperty("RCSBatchPostSyncVA",target,64);
    setProperty("RCSBatchPostSyncPhysical",targetTable,64);
    auto *data=OSData::withBytes(targetEntries,sizeof(targetEntries));
    if(data){setProperty("RCSBatchPostSyncPTEs",data);data->release();}
    if(targetOK&&copyHighRAM(targetTable,pages+4096,4096)){
     data=OSData::withBytes(pages+4096,4096);
     if(data){setProperty("RCSBatchPostSyncPage",data);data->release();}
    }
   }
  }
  uint32_t endLo=0,endHi=0,endHead=0,endReset=~0U;
  stable=stable&&read32(map,kRcsBase+0x270,endLo)&&read32(map,kRcsBase+0x274,endHi)&&
   read32(map,kRcsBase+0x74,endHead)&&read32(map,kRcsBase+0xd0,endReset)&&
   endLo==rootLo&&endHi==rootHi&&endHead==acthd&&endReset==reset&&(endReset&3U)==0;
  IOFree(pages,8192);map->release();
  setProperty("RCSBatchStable",stable);setReg(this,"RCSBatchResult",ok?(stable?0:6):5);
  return ok?kIOReturnSuccess:kIOReturnIOError;
 }
 IOReturn inspectBCS(){
  BCSSnapshotV1 r={};r.version=1;r.action=0;
  IOService *accel=getProvider();
  auto *pci=accel?OSDynamicCast(IOPCIDevice,accel->getProvider()):nullptr;
  if(!pci||pci->isInactive()||pci->configRead16(kIOPCIConfigVendorID)!=kIntelVendor||
     pci->configRead16(kIOPCIConfigDeviceID)!=kADLP46A3||
     !(pci->configRead16(kIOPCIConfigCommand)&2)){r.result=10;return publishBCS(r,kIOReturnNotReady);}
  auto *map=pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0,kIOMapInhibitCache);
  if(!map){r.result=11;return publishBCS(r,kIOReturnNoMemory);}
  bool ok=read32(map,kForceWakeGTAck,r.forceWakeBefore);
  NativeForceWake wake(accel,4);
  uint32_t ack=0;
  if(ok)ok=waitAck(map,kForceWakeGTAck,true,ack);
  r.forceWakeAfterAcquire=ack;
  ok=ok&&read32(map,kRingHead,r.headBefore)&&read32(map,kRingTail,r.tailBefore)&&
     read32(map,kRingStart,r.startBefore)&&read32(map,kRingControl,r.controlBefore)&&
     read32(map,kRingHWS,r.hwsBefore)&&read32(map,kRingPDP0Lo,r.pdp0LoBefore)&&
     read32(map,kRingPDP0Hi,r.pdp0HiBefore)&&read32(map,kRingIPEHR,r.ipehrBefore)&&
     read32(map,kRingACTHD,r.acthdBefore);
  IOReturn status=kIOReturnSuccess;
  if(!ok){r.result=12;status=kIOReturnIOError;}
  const bool after=read32(map,kRingHead,r.headAfter)&&read32(map,kRingTail,r.tailAfter)&&
   read32(map,kRingStart,r.startAfter)&&read32(map,kRingControl,r.controlAfter)&&
   read32(map,kRingHWS,r.hwsAfter)&&read32(map,kRingPDP0Lo,r.pdp0LoAfter)&&
   read32(map,kRingPDP0Hi,r.pdp0HiAfter)&&read32(map,kRingIPEHR,r.ipehrAfter)&&
   read32(map,kRingACTHD,r.acthdAfter);
  if(status==kIOReturnSuccess&&!after){r.result=15;status=kIOReturnIOError;}
  wake.release();
  map->release();return publishBCS(r,status);
 }
 IOReturn publishBCS(const BCSSnapshotV1 &r,IOReturn status){
  auto *data=OSData::withBytes(&r,sizeof(r));if(!data)return kIOReturnNoMemory;
  const bool saved=setProperty("BCSSnapshotV1",data);data->release();
  setReg(this,"BCSProbeResult",r.result);setReg(this,"BCSHeadBefore",r.headBefore);
  setReg(this,"BCSTailBefore",r.tailBefore);setReg(this,"BCSStartBefore",r.startBefore);
  setReg(this,"BCSControlBefore",r.controlBefore);setReg(this,"BCSHWSBefore",r.hwsBefore);
  setReg(this,"BCSPDP0LoBefore",r.pdp0LoBefore);setReg(this,"BCSPDP0HiBefore",r.pdp0HiBefore);
  setReg(this,"BCSIPEHRBefore",r.ipehrBefore);setReg(this,"BCSACTHDBefore",r.acthdBefore);
  setReg(this,"BCSHeadAfter",r.headAfter);setReg(this,"BCSTailAfter",r.tailAfter);
  setReg(this,"BCSControlAfter",r.controlAfter);
  return saved?status:kIOReturnNoMemory;
 }
 IOReturn inspectRCS(bool applyWorkaround,bool captureDepthState=false){
  RCSSnapshotV1 r={};r.version=1;
  rcsWorkaroundLive=false;
  if(captureDepthState){
   removeProperty("DepthContextSnapshotV1");
   removeProperty("RCSGen12StateV1");
   setProperty("RCSGen12StateValid",false);
   setProperty("RCSGen12StateStable",false);
   setProperty("DepthContextSnapshotValid",false);
   setProperty("DepthContextSnapshotStable",false);
  }
  IOService *accel=getProvider();
  auto *pci=accel?OSDynamicCast(IOPCIDevice,accel->getProvider()):nullptr;
  if(!pci||pci->isInactive()||pci->configRead16(kIOPCIConfigVendorID)!=kIntelVendor||
     pci->configRead16(kIOPCIConfigDeviceID)!=kADLP46A3||
     !(pci->configRead16(kIOPCIConfigCommand)&2)){r.result=10;return publishRCS(r,kIOReturnNotReady);}
  if(pci->configRead8(kIOPCIConfigRevisionID)!=0x0c){
   r.result=17;return publishRCS(r,kIOReturnUnsupported);
  }
  auto *map=pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0,kIOMapInhibitCache);
  if(!map){r.result=11;return publishRCS(r,kIOReturnNoMemory);}
  bool ok=read32(map,kForceWakeGTAck,r.gtAckBefore);
  ok=ok&&read32(map,kForceWakeRenderAck,r.renderAckBefore);
  NativeForceWake wake(accel,5);
  if(ok)ok=waitAck(map,kForceWakeGTAck,true,r.gtAckAfterAcquire);
  if(ok)ok=waitAck(map,kForceWakeRenderAck,true,r.renderAckAfterAcquire);
  ok=ok&&read32(map,kRcsBase+kRingPSMI,r.psmiBefore)&&
     read32(map,kRcsBase+0x54,r.unknown2054Before)&&
     read32(map,kRcsBase+0x34,r.headBefore)&&read32(map,kRcsBase+0x30,r.tailBefore)&&
     read32(map,kRcsBase+0x38,r.startBefore)&&read32(map,kRcsBase+0x3c,r.controlBefore)&&
     read32(map,kRcsBase+0x80,r.hwsBefore)&&read32(map,kRcsBase+0x270,r.pdp0LoBefore)&&
     read32(map,kRcsBase+0x274,r.pdp0HiBefore)&&read32(map,kRcsBase+0x68,r.ipehrBefore)&&
     read32(map,kRcsBase+0x74,r.acthdBefore)&&read32(map,kRcsBase+0x234,r.execlistStatusBefore)&&
     read32(map,kRcsBase+0x3a0,r.contextStatusPointerBefore)&&read32(map,kRcsBase+0x180,r.ccidBefore)&&
     read32(map,kRcsBase+0xd0,r.resetControlBefore)&&read32(map,kRcsBase+0x9c,r.miModeBefore)&&
     read32(map,kRcsBase+0xc0,r.instpmBefore)&&read32(map,0x2140,r.reg2140Before)&&
     read32(map,0x4030,r.reg4030Before)&&read32(map,0x4040,r.reg4040Before)&&
     read32(map,0x4044,r.reg4044Before)&&read32(map,0x4050,r.reg4050Before)&&
     read32(map,0xcec4,r.regCEC4Before);
  if(captureDepthState){
   // Observation only. These are per-context hardware settings; a stable CCID
   // makes the snapshot interpretable but does not identify it as Maps' context.
   // Linux gen12_ctx_workarounds_init: Wa_1806527549 and Wa_1606376872.
   uint32_t depth[7]={1,0,0,0,0,0,0};
   const bool valid=ok&&read32(map,0x2180,depth[1])&&read32(map,0x7018,depth[2])&&
    read32(map,0x7300,depth[3])&&read32(map,0x2180,depth[4])&&
    read32(map,0x20d0,depth[5]);
   depth[6]=valid&&depth[1]==depth[4]&&depth[5]==0;
   if(valid){
    auto *data=OSData::withBytes(depth,sizeof(depth));
    if(data){setProperty("DepthContextSnapshotV1",data);data->release();}
   }
   setProperty("DepthContextSnapshotValid",valid);
   setProperty("DepthContextSnapshotStable",depth[6]!=0);
   // Observe Gen12 context settings under the same acquired forcewake.
   // Equality at the endpoints is NOT an atomic snapshot or context ownership
   // proof (including an ABA switch). Preserve the root and ACTHD for correlation
   // with the separately captured command page instead of trusting CCID alone.
   // Do not read FF_MODE2: Wa_1608008084 makes CPU readback unreliable.
   uint32_t gen12[16]={1};
   const bool gen12Valid=ok&&
    read32(map,0x2180,gen12[1])&&read32(map,0x2270,gen12[2])&&
    read32(map,0x2274,gen12[3])&&read32(map,0x2074,gen12[4])&&
    read32(map,0x7010,gen12[5])&&read32(map,0x7304,gen12[6])&&
    read32(map,0x2580,gen12[7])&&read32(map,0x7018,gen12[8])&&
    read32(map,0x7300,gen12[9])&&read32(map,0x2180,gen12[10])&&
    read32(map,0x2270,gen12[11])&&read32(map,0x2274,gen12[12])&&
    read32(map,0x2074,gen12[13])&&read32(map,0x20d0,gen12[14]);
   gen12[15]=gen12Valid&&gen12[1]==gen12[10]&&gen12[2]==gen12[11]&&
    gen12[3]==gen12[12]&&gen12[4]==gen12[13]&&gen12[14]==0;
   bool published=false;
   if(gen12Valid){
    auto *data=OSData::withBytes(gen12,sizeof(gen12));
    if(data){published=setProperty("RCSGen12StateV1",data);data->release();}
   }
   setProperty("RCSGen12StateValid",gen12Valid&&published);
   setProperty("RCSGen12StateStable",published&&gen12[15]!=0);
   setProperty("RCSGen12FFModeReadbackUsable",false);
  }
  IOReturn status=kIOReturnSuccess;
  if(applyWorkaround && ok){
   const bool pristine=r.resetControlBefore==0 && r.headBefore==0 && r.tailBefore==0 &&
     r.startBefore==0 && r.controlBefore==0 && r.acthdBefore==0;
   if(!pristine){map->release();r.result=18;return publishRCS(r,kIOReturnBusy);}
   const auto registered=prepareResetRegistry();
   if(registered!=kIOReturnSuccess){map->release();r.result=19;return publishRCS(r,registered);}
  }
  if(!ok){r.result=12;status=kIOReturnIOError;}
  else if((r.psmiBefore&kADLPRequiredRCSPSMIBits)==kADLPRequiredRCSPSMIBits){
   r.result=applyWorkaround?1U:0U;
  }else if(applyWorkaround){
   const bool idle=r.resetControlBefore==0&&r.headBefore==r.tailBefore&&
      r.headBefore==0&&r.startBefore==0&&r.controlBefore==0&&r.acthdBefore==0;
   if(!idle){r.result=13;status=kIOReturnBusy;}
   else if(!write32(map,kRcsBase+kRingPSMI,
                    (kADLPRequiredRCSPSMIBits<<16U)|kADLPRequiredRCSPSMIBits)){
    r.result=14;status=kIOReturnIOError;
   }else r.result=2;
  }
  uint32_t startAfter=0,controlAfter=0,hwsAfter=0,pdp0LoAfter=0,pdp0HiAfter=0;
  uint32_t resetControlAfter=~0U;
  const bool after=read32(map,kRcsBase+kRingPSMI,r.psmiAfter)&&
     read32(map,kRcsBase+0x34,r.headAfter)&&read32(map,kRcsBase+0x30,r.tailAfter)&&
     read32(map,kRcsBase+0x38,startAfter)&&read32(map,kRcsBase+0x3c,controlAfter)&&
     read32(map,kRcsBase+0x80,hwsAfter)&&read32(map,kRcsBase+0x270,pdp0LoAfter)&&
     read32(map,kRcsBase+0x274,pdp0HiAfter)&&read32(map,kRcsBase+0x68,r.ipehrAfter)&&
     read32(map,kRcsBase+0x74,r.acthdAfter)&&
     read32(map,kRcsBase+0xd0,resetControlAfter);
  if(status==kIOReturnSuccess&&!after){r.result=15;status=kIOReturnIOError;}
  if(applyWorkaround&&status==kIOReturnSuccess&&r.result==2){
   const bool preserved=(r.psmiAfter&~kADLPRequiredRCSPSMIBits)==
                         (r.psmiBefore&~kADLPRequiredRCSPSMIBits);
   const bool enabled=(r.psmiAfter&kADLPRequiredRCSPSMIBits)==kADLPRequiredRCSPSMIBits;
   const bool stillIdle=r.headAfter==r.headBefore&&r.tailAfter==r.tailBefore&&
                        startAfter==r.startBefore&&controlAfter==r.controlBefore&&
                        hwsAfter==r.hwsBefore&&pdp0LoAfter==r.pdp0LoBefore&&
                        pdp0HiAfter==r.pdp0HiBefore&&
                        r.ipehrAfter==r.ipehrBefore&&r.acthdAfter==r.acthdBefore;
   if(!preserved||!enabled||!stillIdle){r.result=16;status=kIOReturnIOError;}
  }
  rcsWorkaroundLive=status==kIOReturnSuccess&&after&&
   r.resetControlBefore==0&&resetControlAfter==0&&
   (r.psmiAfter&kADLPRequiredRCSPSMIBits)==kADLPRequiredRCSPSMIBits;
  wake.release();
  // Other native users may still own references: ACK need not clear here.
  read32(map,kForceWakeRenderAck,r.renderAckAfterRelease);
  read32(map,kForceWakeGTAck,r.gtAckAfterRelease);
  map->release();return publishRCS(r,status,resetControlAfter);
 }
 IOReturn publishRCS(const RCSSnapshotV1 &r,IOReturn status,uint32_t resetControlAfter=~0U){
  auto *data=OSData::withBytes(&r,sizeof(r));if(!data)return kIOReturnNoMemory;
  const bool saved=setProperty("RCSSnapshotV1",data);data->release();
  setReg(this,"RCSProbeResult",r.result);setReg(this,"RCSPSMIBefore",r.psmiBefore);
  setReg(this,"RCSPSMIAfter",r.psmiAfter);
  setReg(this,"RCSPSMIMissingADLPBits",kADLPRequiredRCSPSMIBits&~r.psmiBefore);
  setProperty("RCSADLPPSMIWorkaroundPresent",
              (r.psmiAfter&kADLPRequiredRCSPSMIBits)==kADLPRequiredRCSPSMIBits);
  setReg(this,"RCSHeadBefore",r.headBefore);setReg(this,"RCSTailBefore",r.tailBefore);
  setReg(this,"RCSStartBefore",r.startBefore);setReg(this,"RCSControlBefore",r.controlBefore);
  setReg(this,"RCSIPEHRBefore",r.ipehrBefore);setReg(this,"RCSACTHDBefore",r.acthdBefore);
  setReg(this,"RCSResetControlBefore",r.resetControlBefore);
  setReg(this,"RCSResetControlAfter",resetControlAfter);
  setProperty("RCSADLPWorkaroundPrepared",rcsWorkaroundLive);
  setProperty("RCSADLPWorkaroundLive",rcsWorkaroundLive);
  return saved?status:kIOReturnNoMemory;
 }
 IOReturn route(bool enable,bool observeUnprepared=false,bool native=false){
  if(!enable){
   if(graphicsControl){
    if(!ReimsADLGraphicsControlDestroy(graphicsControl))return kIOReturnBusy;
    graphicsControl=nullptr;
   }
   if(pipeCapabilitiesPublished){
    if(oldPipeCapabilities)getProvider()->setProperty("IOAccelDisplayPipeCapabilities",oldPipeCapabilities);
    else getProvider()->removeProperty("IOAccelDisplayPipeCapabilities");
    OSSafeReleaseNULL(oldPipeCapabilities);pipeCapabilitiesPublished=false;
   }
   if(framebuffer){
    if(oldTypes)framebuffer->setProperty("IOAccelTypes",oldTypes);else framebuffer->removeProperty("IOAccelTypes");
    if(oldIndex)framebuffer->setProperty("IOAccelIndex",oldIndex);else framebuffer->removeProperty("IOAccelIndex");
    framebuffer->release();framebuffer=nullptr;
   }
   OSSafeReleaseNULL(oldTypes);OSSafeReleaseNULL(oldIndex);setProperty("Linked",false);
   rcsWorkaroundLive=false;
   setProperty("RCSADLPWorkaroundPrepared",false);
   setProperty("RCSADLPWorkaroundLive",false);
   setProperty("ObservationLink",false);return kIOReturnSuccess;
  }
  // Never authorise a route using a prior successful observation.  In
  // particular, PSMI is restored by GT reset while this service can survive.
  const IOReturn liveStatus=inspectRCS(false);
  if(liveStatus!=kIOReturnSuccess||!rcsWorkaroundLive){
   if(framebuffer)route(false);
   return liveStatus==kIOReturnSuccess?kIOReturnNotReady:liveStatus;
  }
  // A native display machine that has already adopted the firmware FB cannot
  // be reused for replacement: terminated pipes permanently change its state.
  // Perform the handoff before the first accelerator probe instead.
  if(framebuffer)return native&&!framebuffer->metaCast("ReimsIntelADLFramebuffer")?kIOReturnBusy:kIOReturnSuccess;
  IOService *accel=getProvider();
  if(!accel || accel->isInactive())return kIOReturnNotReady;
  auto *pci=OSDynamicCast(IOPCIDevice,accel->getProvider());
  if(!pci||pci->configRead16(kIOPCIConfigVendorID)!=kIntelVendor||pci->configRead16(kIOPCIConfigDeviceID)!=kADLP46A3)return kIOReturnUnsupported;
  IOService *fb=nullptr;
  auto *it=pci->getChildIterator(gIOServicePlane);if(!it)return kIOReturnNoMemory;
  while(auto *obj=it->getNextObject()){
   auto *candidate=OSDynamicCast(IOService,obj);
   if(candidate && candidate->metaCast("IONDRVFramebuffer") && candidate->getProvider()==pci){
    if(fb){fb->release();it->release();return kIOReturnUnsupported;}
    fb=candidate;fb->retain();
   }
  }
  it->release();if(!fb)return kIOReturnNotFound;
  char path[1024];int len=sizeof(path);
  if(!accel->getPath(path,&len,gIOServicePlane)){fb->release();return kIOReturnError;}
  oldTypes=fb->copyProperty("IOAccelTypes");oldIndex=fb->copyProperty("IOAccelIndex");framebuffer=fb;
  // IOAccelDisplayPipe::copyCapabilities reads this accelerator property.
  // The standalone personality omitted it, yielding success with no serialized
  // object from selector 2. These describe the existing native TGL pipe and
  // transaction implementations; their actual errors/completion are unchanged.
  auto *caps=OSDictionary::withCapacity(2);
  auto *one=OSNumber::withNumber(1ULL,32);
  if(!caps||!one){OSSafeReleaseNULL(caps);OSSafeReleaseNULL(one);route(false);return kIOReturnNoMemory;}
  bool capsOK=caps->setObject("DisplayPipeSupported",one)&&caps->setObject("TransactionsSupported",one);
  oldPipeCapabilities=accel->copyProperty("IOAccelDisplayPipeCapabilities");
  pipeCapabilitiesPublished=true;
  capsOK=capsOK&&accel->setProperty("IOAccelDisplayPipeCapabilities",caps);
  caps->release();one->release();
  if(!capsOK){route(false);return kIOReturnNoMemory;}
  if(native){
   auto*replacement=ReimsIntelFramebufferCreate(accel,OSDynamicCast(IOFramebuffer,fb));
   if(!replacement){route(false);return kIOReturnNotReady;}
   framebuffer=replacement;fb->release();fb=replacement;
   OSSafeReleaseNULL(oldTypes);OSSafeReleaseNULL(oldIndex);
  }else if(!fb->setProperty("IOAccelTypes",path)||!fb->setProperty("IOAccelIndex",(UInt64)0,32)){
   route(false);return kIOReturnNoMemory;
  }
  graphicsControl=ReimsADLGraphicsControlCreate(accel,fb);
  if(!graphicsControl){route(false);return kIOReturnNotReady;}
  setProperty("AcceleratorPath",path);setProperty("Linked",true);
  setProperty("NativeFramebuffer",native);
  setProperty("ObservationLink",observeUnprepared);return kIOReturnSuccess;
 }
 IOReturn nativeFramebuffer(){
  return route(true,false,true);
 }
public:
 bool start(IOService *p) override {
  if(!IOService::start(p))return false;
  if(p->getProperty("ReimsADLOffscreenBringup")!=kOSBooleanTrue)return false;
  lock=IOLockAlloc();if(!lock)return false;
  setProperty("Linked",false);setProperty("ObservationLink",false);
  setProperty("RCSADLPWorkaroundPrepared",false);
  setProperty("RCSADLPWorkaroundLive",false);
  setProperty("BCSProbePolicy","read-only-exact-46a3-no-ring-control-write");
  setProperty("RCSProbePolicy","read-only-capture-or-explicit-idle-masked-Wa_1607297627-exact-46a3");
  setProperty("ReimsForceWakeOwnership","native-refcounted");
  setProperty("ReimsBacklightAttached",ReimsAttachBacklight(getProvider()));
  registerService();return true;
 }
 IOReturn setProperties(OSObject *props) override {
  if(IOUserClient::clientHasPrivilege(current_task(),kIOClientPrivilegeAdministrator)!=kIOReturnSuccess)return kIOReturnNotPrivileged;
  auto *d=OSDynamicCast(OSDictionary,props);if(!d||!lock)return kIOReturnBadArgument;
  auto *capture=OSDynamicCast(OSBoolean,d->getObject("CaptureBCS"));
  auto *captureRCS=OSDynamicCast(OSBoolean,d->getObject("CaptureRCS"));
  auto *captureDepth=OSDynamicCast(OSBoolean,d->getObject("CaptureDepthState"));
  auto *captureBatch=OSDynamicCast(OSBoolean,d->getObject("CaptureRCSBatch"));
  auto *prepareRCS=OSDynamicCast(OSBoolean,d->getObject("PrepareRCSADLP"));
  auto *observe=OSDynamicCast(OSBoolean,d->getObject("ObserveLink"));
  auto *link=OSDynamicCast(OSBoolean,d->getObject("Link"));
  IOLockLock(lock);IOReturn r=kIOReturnBadArgument;
  if(d->getObject("NativeFramebuffer")==kOSBooleanTrue)r=nativeFramebuffer();
  else if(d->getObject("CaptureRPS")==kOSBooleanTrue)r=captureRPS();
  else if(captureBatch==kOSBooleanTrue)r=captureRCSBatch();
  else if(capture==kOSBooleanTrue)r=inspectBCS();
  else if(captureRCS==kOSBooleanTrue)r=inspectRCS(false);
  else if(captureDepth==kOSBooleanTrue)r=inspectRCS(false,true);
  else if(prepareRCS==kOSBooleanTrue)r=inspectRCS(true);
  else if(observe==kOSBooleanTrue)r=route(true,true);
  else if(link)r=route(link->getValue());
  IOLockUnlock(lock);return r;
 }
 void stop(IOService *p) override {if(lock){IOLockLock(lock);route(false);IOLockUnlock(lock);}IOService::stop(p);}
 void free() override {if(lock){IOLockFree(lock);lock=nullptr;}IOService::free();}
};
OSDefineMetaClassAndStructors(ReimsADLDesktopLink,IOService)
