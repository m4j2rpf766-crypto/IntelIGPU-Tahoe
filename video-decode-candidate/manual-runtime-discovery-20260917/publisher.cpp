#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSBoolean.h>
class ReimsVideoDiscovery:public IOService {
 OSDeclareDefaultStructors(ReimsVideoDiscovery)
 OSDictionary *saved=nullptr,*published=nullptr;
 void restore(IOService*p){
  if(!published||!saved)return;
  auto*it=OSCollectionIterator::withCollection(published);
  if(!it)return;
  while(auto*k=OSDynamicCast(OSString,it->getNextObject())){
   // Do not undo changes subsequently made by another owner.
   if(p->getProperty(k)!=published->getObject(k))continue;
   if(auto*old=saved->getObject(k))p->setProperty(k,old);else p->removeProperty(k);
  }it->release();
 }
public:
 bool start(IOService*p)override{
  if(!IOService::start(p))return false;
  if(p->getProperty("ReimsADLOffscreenBringup")!=kOSBooleanTrue)return false;
  // Registry device-id is intentionally FFFF for boot isolation. Verify the
  // actual PCI function without changing the isolated registry identity.
  auto*pci=OSDynamicCast(IOPCIDevice,p->getProvider());
  if(!pci||pci->isInactive()||
     pci->configRead16(kIOPCIConfigVendorID)!=0x8086||
     pci->configRead16(kIOPCIConfigDeviceID)!=0x46a3||
     pci->configRead8(kIOPCIConfigRevisionID)!=0x0c)return false;
  auto*props=OSDynamicCast(OSDictionary,getProperty("VideoProperties"));if(!props)return false;
  saved=OSDictionary::withCapacity(16);published=OSDictionary::withDictionary(props);
  if(!saved||!published)return false;
  auto*it=OSCollectionIterator::withCollection(published);if(!it)return false;
  while(auto*k=OSDynamicCast(OSString,it->getNextObject())){
   const char*s=k->getCStringNoCopy();
   if(strncmp(s,"IOGVA",5)&&strcmp(s,"IODVDBundleName")&&strcmp(s,"IOVARendererID")){it->release();return false;}
   if(auto*old=p->getProperty(k))saved->setObject(k,old);
  }it->reset();
  while(auto*k=OSDynamicCast(OSString,it->getNextObject()))p->setProperty(k,published->getObject(k));
  it->release();setProperty("PhysicalIdentityVerified",true);setProperty("Published",true);registerService();return true;
 }
 void stop(IOService*p)override{restore(p);IOService::stop(p);}
 void free()override{OSSafeReleaseNULL(saved);OSSafeReleaseNULL(published);IOService::free();}
};
OSDefineMetaClassAndStructors(ReimsVideoDiscovery,IOService)
