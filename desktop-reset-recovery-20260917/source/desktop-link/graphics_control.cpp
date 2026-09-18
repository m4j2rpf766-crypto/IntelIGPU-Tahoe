#include "graphics_control.hpp"
#include "graphics_control_pipeline.hpp"
#include <IOKit/IOLib.h>
#include <libkern/OSAtomic.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/graphics/IOFramebuffer.h>
OSDefineMetaClassAndStructors(ReimsADLGraphicsControl, AppleGraphicsDeviceControl)

bool ReimsADLGraphicsControl::setFramebuffer(IOService* fb) {
 if (!fb || framebuffer || !fb->getRegistryEntryID() || !fb->metaCast("IOFramebuffer")) return false;
 framebuffer = fb; framebuffer->retain();
 return true;
}
bool ReimsADLGraphicsControl::terminate(IOOptionBits options) {
 // Apple's base refuses termination while its provider is active unless its
 // private hotplug flag is set (Ventura KC 0x1442a7c6). Our service explicitly
 // supports removal of this runtime association while the GPU remains alive.
 // Use the normal IOKit lifecycle; inherited AGDC::stop still unregisters its
 // plugin and removes the real node-map entry before detachment.
 return IOService::terminate(options);
}
void ReimsADLGraphicsControl::free() {
 if (framebuffer) { framebuffer->release(); framebuffer = nullptr; }
 AppleGraphicsDeviceControl::free();
}
IOReturn ReimsADLGraphicsControl::vendor_doDeviceAttribute(UInt32 selector,
 unsigned long* input, unsigned long inputSize, unsigned long* output,
 unsigned long* outputSize, IOExternalMethodArguments*) {
 (void)input; (void)inputSize;
 // AGDC's filtered API consumes kernel pointers here and translates them to
 // registry IDs before copying the node map to userspace. Never place IDs in
 // these pointer fields, and never expose this method directly as a user ABI.
 if (selector == 0x711) {
  if (!framebuffer || !getProvider()) return kIOReturnNotReady;
  IOReturn result=ReimsADLGraphicsControlPipelineCapabilities(input,inputSize,output,outputSize);
  static volatile SInt32 samples=0;
  if(OSIncrementAtomic(&samples)<=32){
   UInt32 words[3]={};if(input&&inputSize>=12)memcpy(words,input,12);
   IOLog("ReimsADLGraphicsControl pipeline connector=%u query=0x%x count=%u result=0x%x\n",words[0],words[1],words[2],result);
  }
  return result;
 }
 if (selector == 0x921) {
  // Ventura AGDC logical-device record, proven from AppleParavirtGPUControl
  // 0x150e051e and IOPresentment::__GatherAGDCLogicalDeviceCapabilities.
  // One logical device (index zero), querying the current boot framebuffer.
  // Optional cursor/scaling/rotation/color capabilities are not advertised.
  if (!input || inputSize != 176 || !output || !outputSize || *outputSize < 176)
   return kIOReturnBadArgument;
  UInt32 index = 0;
  memcpy(&index, input, 4);
  if (index) return kIOReturnBadArgument;
  bzero(output, 176);
  auto bytes = reinterpret_cast<unsigned char*>(output);
  const UInt64 versionFlags = 1, nativeUnit = 1;
  auto* fb=OSDynamicCast(IOFramebuffer,framebuffer);
  IODisplayModeID mode=0;IOIndex depth=0;
  IOPixelInformation pixel={};
  if(!fb||fb->getCurrentDisplayMode(&mode,&depth)!=kIOReturnSuccess||
     fb->getPixelInformation(mode,depth,kIOFBSystemAperture,&pixel)!=kIOReturnSuccess||
     pixel.bitsPerPixel!=32||!pixel.activeWidth||!pixel.activeHeight)
   return kIOReturnNotReady;
  const UInt32 width=pixel.activeWidth, height=pixel.activeHeight;
  memcpy(bytes + 8, &versionFlags, 8);
  memcpy(bytes + 28, &width, 4);
  memcpy(bytes + 32, &height, 4);
  memcpy(bytes + 68, &width, 4);  // horizontalActive
  memcpy(bytes + 84, &height, 4); // verticalActive
  memcpy(bytes + 132, &nativeUnit, 8);
  IOTimingInformation timing={};timing.flags=kIODetailedTimingValid;
  if(fb->getTimingInfoForDisplayMode(mode,&timing)==kIOReturnSuccess&&
     (timing.flags&kIODetailedTimingValid)&&timing.detailedInfo.v2.pixelClock){
   const auto&t=timing.detailedInfo.v2;
   // AGDC's packed record embeds the timing fields at +4 (the APV ABI
   // above has horizontalActive at +68 and verticalActive at +84).
   memcpy(bytes+44,&t.pixelClock,8);memcpy(bytes+52,&t.minPixelClock,8);memcpy(bytes+60,&t.maxPixelClock,8);
   memcpy(bytes+68,&t.horizontalActive,4);memcpy(bytes+72,&t.horizontalBlanking,4);
   memcpy(bytes+76,&t.horizontalSyncOffset,4);memcpy(bytes+80,&t.horizontalSyncPulseWidth,4);
   memcpy(bytes+84,&t.verticalActive,4);memcpy(bytes+88,&t.verticalBlanking,4);
   memcpy(bytes+92,&t.verticalSyncOffset,4);memcpy(bytes+96,&t.verticalSyncPulseWidth,4);
   memcpy(bytes+116,&t.horizontalSyncConfig,4);memcpy(bytes+124,&t.verticalSyncConfig,4);
  }
  // If IONDRV has no detailed timing, leave that record absent rather than
  // inventing a hardware pixel clock. IOP applies its documented-in-code
  // default timing; this is not a claim that physical vblank is integrated.
  *outputSize = 176;
  return kIOReturnSuccess;
 }
 if (selector != 1 && selector != 0x980) {
  // Bound diagnostics even if a client continuously retries an unsupported
  // attribute. Record sizes and the connector word, never kernel pointers or
  // request payloads; this identifies the next real schema required by WS.
  UInt32 ticket;
  do {
   ticket = unsupportedLogCount;
   if (ticket >= 64) return kIOReturnUnsupported;
  } while (!OSCompareAndSwap(ticket, ticket + 1, &unsupportedLogCount));
  UInt32 connectorWord = 0;
  if (input && inputSize >= sizeof(connectorWord))
   memcpy(&connectorWord, input, sizeof(connectorWord));
  IOLog("ReimsADLGraphicsControl: unsupported attribute=0x%x input=%lu output=%lu firstWord=0x%x sample=%u/64\n",
        selector, inputSize, outputSize ? *outputSize : 0,
        connectorWord, ticket + 1);
  return kIOReturnUnsupported;
 }
 const unsigned long required = selector == 1 ? 44 : 220;
 if (!outputSize || !output || *outputSize < required) return kIOReturnBadArgument;
 if (selector == 0x980 && (!framebuffer || !getProvider())) return kIOReturnNotReady;
 bzero(output, required);
 auto bytes = reinterpret_cast<unsigned char*>(output);
 if (selector == 1) {
  const UInt32 version = 0x10000, deviceType = 1;
  IOPCIDevice* pci = nullptr;
  for (IOService* p = getProvider(); p; p = p->getProvider()) {
   pci = OSDynamicCast(IOPCIDevice, p);
   if (pci) break;
  }
  if (!pci) return kIOReturnNotReady;
  const UInt32 pciIdentity = pci->configRead16(kIOPCIConfigVendorID);
  memcpy(bytes, &version, 4);
  const char name[] = "Reims ADL-P integrated GPU";
  memcpy(bytes + 4, name, sizeof(name));
  memcpy(bytes + 36, &pciIdentity, 4);
  memcpy(bytes + 40, &deviceType, 4);
 } else {
  // Connector IDs begin at one in AGDC: one connected connector => bit 1.
  const UInt64 mask = 2;
  const UInt32 count = 1;
  for (unsigned i = 0; i < 4; ++i) memcpy(bytes + 8*i, &mask, 8);
  for (unsigned i = 0; i < 5; ++i) memcpy(bytes + 32 + 4*i, &count, 4);
  IOService* provider = getProvider();
  memcpy(bytes + 52, &provider, sizeof(provider));
  memcpy(bytes + 60, &framebuffer, sizeof(framebuffer));
 }
 *outputSize = required;
 return kIOReturnSuccess;
}

ReimsADLGraphicsControl* ReimsADLGraphicsControlCreate(IOService* gpu, IOService* fb) {
 if (!gpu || !fb || !gpu->getRegistryEntryID()) return nullptr;
 auto control = new ReimsADLGraphicsControl;
 if (!control) return nullptr;
 if (!control->init() || !control->setFramebuffer(fb)) { control->release(); return nullptr; }
 if (!control->attach(gpu)) { control->release(); return nullptr; }
 if (!control->start(gpu)) { control->detach(gpu); control->release(); return nullptr; }
 // Base start() already calls registerService after inserting the real node map.
 return control;
}
bool ReimsADLGraphicsControlDestroy(ReimsADLGraphicsControl* control) {
 if (!control) return true;
 if (!control->terminate(kIOServiceRequired | kIOServiceSynchronous)) {
  IOLog("ReimsADLGraphicsControl: termination refused; caller retains service ownership\n");
  return false;
 }
 control->release();
 return true;
}
