#pragma once

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSObject.h>
#include "physical_alias.hpp"

// Native TGL calls these inherited virtual slots. Symbol-table presence is not
// an export guarantee: Ventura AuxKC rejects direct linking to getLength().
inline uint64_t ReimsIOAccelMapValue(OSObject*object,size_t byteSlot){
 auto*table=*reinterpret_cast<const uintptr_t*const*>(object);
 return reinterpret_cast<uint64_t(*)(OSObject*)>(table[byteSlot/sizeof(uintptr_t)])(object);
}

// Ventura 13.7.8 / AppleIntelTGLGraphics 16.0.0 diagnostic only.
// No MMIO writes. Descriptor preparation here pins a bounded read-only
// diagnostic snapshot; it never substitutes for GPU completion.
namespace ReimsNativeMapping {

constexpr uint64_t kPageSize = 4096;
constexpr uint64_t kGGTTBAR0Offset = 0x800000;
constexpr uint64_t kADLPTEAddressMask = 0x0000fffffffff000ULL;
constexpr uint32_t kMaximumDiagnosticPages = 4096;

enum class Knowledge : uint8_t { Unknown, No, Yes };
enum class ResultCode : uint32_t {
 Success,
 BadArgument,
 WrongResourceClass,
 MissingNativeMap,
 WrongMapClass,
 MissingNativeMemory,
 BackingDescriptorMismatch,
 MissingOwner,
 WrongOwnerClass,
 NativeRangeMismatch,
 BAR0RangeMismatch,
 IOMapperActive,
 DescriptorTooShort,
 DescriptorHasNoPhysicalSegment,
 GGTTEntryNotPresent,
 GGTTEntryUnstable,
 GGTTPhysicalMismatch,
 TooManyPages,
 WrongSysMemoryClass,
 MissingNativeDescriptor,
 ResourceOffsetUnsupported,
 RemappedLayoutUnsupported,
 DescriptorPrepareFailed,
 DescriptorPhysicalMismatch,
 MappingChanged,
 NativeMemoryNotWired,
 UnsupportedStorageResource,
 MissingSurfaceBacking,
 SurfaceBackingUnrelated,
 NativeMemoryUnrelated,
 PhysicalAddressOutOfRange,
};

struct Result {
 ResultCode code = ResultCode::BadArgument;
 Knowledge pageTableListSafe = Knowledge::Unknown;
 Knowledge hasGlobalPageTable = Knowledge::Unknown;
 Knowledge ggttMatchesDescriptor = Knowledge::Unknown;
 const char *resourceClass = nullptr;
 const char *mapClass = nullptr;
 const char *memoryClass = nullptr;
 const char *ownerClass = nullptr;
 const char *pageTableClass = nullptr;
 const char *nativeDescriptorClass = nullptr;
 const char *surfaceDescriptorClass = nullptr;
 uint64_t nativeDescriptorLength = 0,surfaceDescriptorLength = 0;
 uint64_t resourceBackingOffset = 0,nativeDescriptorFirstPhysical = 0;
 uint32_t aliasPagesChecked = 0,prepareStatus = 0,mapFlags = 0;
 uint32_t memoryFlags=0;
 uint32_t resourceType=0;
 bool descriptorIdentity = false,physicalAlias = false;
 bool surfaceBackingIdentity=false,nativeUsesResident=false;
 uint32_t backingRelation=0; // 1 shared physical pages, 2 distinct resident/backing pair
 uint64_t storageDescriptorLength=0,aliasMismatchPage=~0ULL;
 uint64_t requestedGPUVA = 0;
 uint64_t nativeGPUVA = 0;
 uint64_t nativeLength = 0;
 uint64_t assignedLength = 0;
 uint64_t requestedBytes = 0;
 uint64_t firstPTE = 0;
 uint64_t firstDescriptorPhysical = 0;
 uint64_t mismatchPage = ~0ULL;
 uint32_t pagesChecked = 0;
 bool cachedProof=false;
};

// Requires the caller's continuous native resource prepare for the proof's
// entire lifetime. Retain alone is insufficient; unpin invalidates the proof.
struct PreparedProof {uint64_t identity[25]={};Result result;bool valid=false;};

struct Retained {
 OSObject*object;
 explicit Retained(OSObject*p):object(p){if(object)object->retain();}
 ~Retained(){if(object)object->release();}
 Retained(const Retained&)=delete;
};
struct Pinned {
 IOMemoryDescriptor*descriptor;
 IOReturn status;
 explicit Pinned(IOMemoryDescriptor*d):descriptor(d),status(d->prepare(kIODirectionNone)){}
 ~Pinned(){if(status==kIOReturnSuccess)descriptor->complete(kIODirectionNone);}
 Pinned(const Pinned&)=delete;
};

inline const char *className(const OSObject *object) {
 if (!object) return nullptr;
 const OSMetaClass *meta = object->getMetaClass();
 return meta ? meta->getClassName() : nullptr;
}

inline bool classIs(const OSObject *object, const char *wanted) {
 const char *actual = className(object);
 return actual && wanted && !strcmp(actual, wanted);
}

inline bool addFits(uint64_t base, uint64_t size, uint64_t limit) {
 return base <= limit && size <= limit - base;
}

inline bool mapperIsActive() {
 OSDictionary *matching = IOService::serviceMatching("IOMapper");
 if (!matching) return true; // Failure to establish absence is not permission.
 IOService *mapper = IOService::copyMatchingService(matching);
 matching->release();
 if (!mapper) return false;
 mapper->release();
 return true;
}

inline Result inspect(OSObject *verifiedResource,
                      IOMemoryDescriptor *backing,
                      IOMemoryMap *bar0,
                      uint64_t gpuVA,
                      uint64_t requiredBytes,PreparedProof*prepared=nullptr) {
 // Any failed inspection breaks the proof epoch, including an early return
 // before identity construction (e.g. unwired or missing mapping).
 const bool hadPreparedProof=prepared&&prepared->valid;
 if(prepared)prepared->valid=false;
 Result out;
 out.requestedGPUVA = gpuVA;
 out.requestedBytes = requiredBytes;
 out.resourceClass = className(verifiedResource);
 if (!verifiedResource || !backing || !bar0 || !requiredBytes ||
     (gpuVA & (kPageSize - 1))) return out;
 if (!classIs(verifiedResource, "IGAccelResource")) {
  out.code = ResultCode::WrongResourceClass;
  return out;
 }

 // IOAccelResource2::fMemoryMap at +0x40, established by its map/prepare path.
 auto *resourceBytes = reinterpret_cast<const uint8_t *>(verifiedResource);
 auto *nativeMap = *reinterpret_cast<OSObject *const *>(resourceBytes + 0x40);
 if (!nativeMap) { out.code = ResultCode::MissingNativeMap; return out; }
 out.mapClass = className(nativeMap);
 if (!classIs(nativeMap, "IGAccelMemoryMap")) {
  out.code = ResultCode::WrongMapClass;
  return out;
 }
 Retained holdMap(nativeMap);

 // +d0 is defined for IOAccelSysMemory, NOT arbitrary IOAccelMemory.
 auto *mapBytes = reinterpret_cast<const uint8_t *>(nativeMap);
 auto *nativeMemory = *reinterpret_cast<OSObject *const *>(mapBytes + 0x18);
 if (!nativeMemory||!nativeMemory->metaCast("IOAccelMemory")) { out.code = ResultCode::MissingNativeMemory; return out; }
 out.memoryClass = className(nativeMemory);
 if(!nativeMemory->metaCast("IOAccelSysMemory")){out.code=ResultCode::WrongSysMemoryClass;return out;}
 Retained holdMemory(nativeMemory);
 auto *memoryBytes = reinterpret_cast<const uint8_t *>(nativeMemory);
 auto *nativeBacking = *reinterpret_cast<IOMemoryDescriptor *const *>(memoryBytes + 0xd0);
 if(!nativeBacking||!nativeBacking->metaCast("IOMemoryDescriptor")){out.code=ResultCode::MissingNativeDescriptor;return out;}
 Retained holdNativeBacking(nativeBacking),holdSurfaceBacking(backing);
 out.nativeDescriptorClass=className(nativeBacking);out.surfaceDescriptorClass=className(backing);
 out.descriptorIdentity=nativeBacking==backing;
 out.nativeDescriptorLength=nativeBacking->getLength();out.surfaceDescriptorLength=backing->getLength();
 out.resourceType=resourceBytes[0x14];
 if(out.resourceType==0x82){uint32_t offset=0;memcpy(&offset,resourceBytes+0xfc,4);out.resourceBackingOffset=offset;}
 else memcpy(&out.resourceBackingOffset,resourceBytes+0xf8,8);
 memcpy(&out.mapFlags,mapBytes+0x10,4);
 memcpy(&out.memoryFlags,memoryBytes+0xc,4);

 // Resource backing(+80) and resident memory(+88) are separate owners.
 // prepareInTaskWithOption1536b5cd chooses +88 if present, else +80.
 // newResourceWithIOSurfaceDeviceCache153667ed stores the original surface
 // descriptor in +80->SysMemory+d0. Type82 resolves storage through +e0.
 OSObject*storage=verifiedResource;
 if(out.resourceType==0x82)storage=*reinterpret_cast<OSObject*const*>(resourceBytes+0xe0);
 if(!storage||!classIs(storage,"IGAccelResource")){
  out.code=ResultCode::UnsupportedStorageResource;return out;
 }
 Retained holdStorage(storage);
 const auto*storageBytes=reinterpret_cast<const uint8_t*>(storage);
 if(storageBytes[0x14]!=0xc0){out.code=ResultCode::UnsupportedStorageResource;return out;}
 auto*surfaceMemory=*reinterpret_cast<OSObject*const*>(storageBytes+0x80);
 if(!surfaceMemory||!surfaceMemory->metaCast("IOAccelSysMemory")){
  out.code=ResultCode::MissingSurfaceBacking;return out;
 }
 Retained holdSurfaceMemory(surfaceMemory);
 const auto*surfaceMemoryBytes=reinterpret_cast<const uint8_t*>(surfaceMemory);
 auto*storageBacking=*reinterpret_cast<IOMemoryDescriptor*const*>(surfaceMemoryBytes+0xd0);
 if(!storageBacking||!storageBacking->metaCast("IOMemoryDescriptor")){
  out.code=ResultCode::MissingSurfaceBacking;return out;
 }
 Retained holdStorageBacking(storageBacking);
 out.storageDescriptorLength=storageBacking->getLength();
 out.surfaceBackingIdentity=storageBacking==backing;
 // Narrow recovered constructor: original IOSurface MD must still be owned by
 // this storage resource. Length equality alone never establishes association.
 if(!out.surfaceBackingIdentity){out.code=ResultCode::SurfaceBackingUnrelated;return out;}
 auto*residentMemory=*reinterpret_cast<OSObject*const*>(resourceBytes+0x88);
 auto*selectedMemory=residentMemory?residentMemory:*reinterpret_cast<OSObject*const*>(resourceBytes+0x80);
 if(nativeMemory!=selectedMemory){out.code=ResultCode::NativeMemoryUnrelated;return out;}
 out.nativeUsesResident=residentMemory!=nullptr;

 // IOAccelMemoryMap::fTask at +0x90. Merely inspect its OSMetaClass here.
 auto *owner = *reinterpret_cast<OSObject *const *>(mapBytes + 0x90);
 if (!owner) { out.code = ResultCode::MissingOwner; return out; }
 out.ownerClass = className(owner);
 if (!classIs(owner, "IGAccelTask")) {
  out.code = ResultCode::WrongOwnerClass;
  return out;
 }

 out.nativeGPUVA = ReimsIOAccelMapValue(nativeMap,0x128);
 // Object vptr points 16 bytes past the full C++ vtable symbol. Actual TGL
 // vptr+168 is getLength. vptr+178 RELEASES PTEs and must never be used here.
 // The assigned-range accessor's recovered implementation reads map+c8.
 memcpy(&out.assignedLength,mapBytes+0xc8,8);
 out.nativeLength = ReimsIOAccelMapValue(nativeMap,0x168);
 // This narrow diagnostic only proves a mapping from its base. Subrange maps
 // have an additional descriptor offset which is not part of the recovered
 // contract, so reject them instead of guessing it.
 // Native commitRange bounds against getLength, not the optional separately
 // assigned range. The live full-size map has length8355840 and assigned0.
 // Assigned0 is not evidence that the map has no backing. Physical comparison
 // below is read-only and does not claim ownership of a new VA reservation.
 if (gpuVA != out.nativeGPUVA || requiredBytes > out.nativeLength) {
  out.code = ResultCode::NativeRangeMismatch;
  return out;
 }
 // Native commitRange uses a special page iterator when +11c is set.
 // Comparing its GGTT against monotonically increasing MD offsets is invalid.
 if(mapBytes[0x11c]){out.code=ResultCode::RemappedLayoutUnsupported;return out;}
 // Full-size base-backed presentation is the recovered contract. Do not
 // invent where a nonzero resource backing offset was folded into the map.
 if(out.resourceBackingOffset){out.code=ResultCode::ResourceOffsetUnsupported;return out;}

 // IGAccelTask+0x268 is a private intrusive queue. No lock or immutable
 // lifetime was found for traversal from the framebuffer callback, so do not
 // dereference it. Page-table identity and hasGlobalPageTable remain unknown.
 out.pageTableListSafe = Knowledge::Unknown;
 out.hasGlobalPageTable = Knowledge::Unknown;

 if (mapperIsActive()) { out.code = ResultCode::IOMapperActive; return out; }
 if (requiredBytes > out.surfaceDescriptorLength||requiredBytes>out.nativeDescriptorLength) {
  out.code = ResultCode::DescriptorTooShort;
  return out;
 }
 if(requiredBytes>uint64_t(kMaximumDiagnosticPages)*kPageSize){out.code=ResultCode::TooManyPages;return out;}
 const uint64_t pages = (requiredBytes + kPageSize - 1) / kPageSize;
 if (!pages || pages > kMaximumDiagnosticPages) {
  out.code = ResultCode::TooManyPages;
  return out;
 }
 // The native transaction has prepared this SysMemory before invoking flip.
 // Require its actual wired flag (IOAccelSysMemory::wire 153999af). The
 // composite descriptor may not support independent preparation: borrow the
 // transaction's wire while its callback owns the resource, never complete it.
 memcpy(&out.memoryFlags,memoryBytes+0xc,4);
 if(!(out.memoryFlags&2)){out.code=ResultCode::NativeMemoryNotWired;return out;}
 const uint64_t identity[]={uintptr_t(verifiedResource),uintptr_t(backing),uintptr_t(nativeMap),
  uintptr_t(nativeMemory),uintptr_t(nativeBacking),uintptr_t(storage),uintptr_t(storageBacking),
  uintptr_t(residentMemory),uintptr_t(owner),gpuVA,out.nativeLength,out.assignedLength,
  requiredBytes,out.resourceBackingOffset,out.mapFlags,out.memoryFlags,out.resourceType,
  uintptr_t(surfaceMemory),uintptr_t(selectedMemory),out.surfaceDescriptorLength,
  out.nativeDescriptorLength,out.storageDescriptorLength,uintptr_t(bar0),
  bar0->getLength(),bar0->getVirtualAddress()};
 static_assert(sizeof(identity)==sizeof(prepared->identity),"proof identity size mismatch");
 if(hadPreparedProof&&!memcmp(prepared->identity,identity,sizeof(identity))){
  out=prepared->result;out.cachedProof=true;out.pagesChecked=0;out.aliasPagesChecked=0;
  prepared->valid=true;return out;
 }
 if(prepared)prepared->valid=false;
 // Pin the external IOSurface descriptor with its declared direction, compare
 // every visible/padded page rather than pointer identity or a first-page test.
 Pinned surfacePin(backing);
 if(surfacePin.status){out.prepareStatus=surfacePin.status;out.code=ResultCode::DescriptorPrepareFailed;return out;}
 auto read=[](IOMemoryDescriptor*d,uint64_t offset){
  IOByteCount length=0;const uint64_t address=d->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
  return ReimsPhysicalAlias::Segment{address,length};
 };
 const auto proof=ReimsPhysicalAlias::compare(requiredBytes,
  [&](uint64_t off){return read(nativeBacking,off);},[&](uint64_t off){return read(backing,off);});
 out.nativeDescriptorFirstPhysical=proof.firstA;out.firstDescriptorPhysical=proof.firstB;
 out.aliasPagesChecked=uint32_t(proof.pages);out.aliasMismatchPage=proof.mismatch;
 if(proof.status!=ReimsPhysicalAlias::Status::Equal&&
    proof.status!=ReimsPhysicalAlias::Status::DifferentPages){
  out.code=ResultCode::DescriptorHasNoPhysicalSegment;return out;
 }
 out.physicalAlias=proof.status==ReimsPhysicalAlias::Status::Equal;
 if(!out.physicalAlias&&!out.nativeUsesResident){
  out.code=ResultCode::DescriptorPhysicalMismatch;return out;
 }
 // Distinct resident pages are legitimate only through the verified resource
 // ownership above. This does NOT assert CPU/GPU contents are synchronized.
 out.backingRelation=out.physicalAlias?1:2;
 const uint64_t firstIndex = gpuVA / kPageSize;
 const uint64_t pteBytes = pages * sizeof(uint64_t);
 if (firstIndex > (~0ULL - kGGTTBAR0Offset) / sizeof(uint64_t)) {
  out.code = ResultCode::BAR0RangeMismatch;
  return out;
 }
 const uint64_t pteOffset = kGGTTBAR0Offset + firstIndex * sizeof(uint64_t);
 if (!addFits(pteOffset, pteBytes, bar0->getLength())) {
  out.code = ResultCode::BAR0RangeMismatch;
  return out;
 }
 auto *mmio = reinterpret_cast<volatile const uint8_t *>(bar0->getVirtualAddress());
 if (!mmio) { out.code = ResultCode::BAR0RangeMismatch; return out; }

 for (uint64_t page = 0; page < pages; ++page) {
  IOByteCount segmentLength = 0;
  const IOByteCount descriptorOffset = static_cast<IOByteCount>(page * kPageSize);
  const addr64_t segment = nativeBacking->getPhysicalSegment(descriptorOffset,
                                                       &segmentLength,
                                                       kIOMemoryMapperNone);
  const uint64_t remaining=requiredBytes-descriptorOffset;
  if (!segment || (segment&(kPageSize-1)) || segmentLength<(remaining<kPageSize?remaining:kPageSize)) {
   out.code = ResultCode::DescriptorHasNoPhysicalSegment;
   out.mismatchPage = page;
   return out;
  }
  // Never truncate an out-of-range physical address into a matching PTE.
  if(segment & ~kADLPTEAddressMask){
   out.code=ResultCode::PhysicalAddressOutOfRange;out.mismatchPage=page;return out;
  }
  const uint64_t expected = static_cast<uint64_t>(segment) & kADLPTEAddressMask;
  const auto *entryAddress = reinterpret_cast<volatile const uint32_t *>(
      mmio + pteOffset + page * sizeof(uint64_t));
  const uint64_t pte = static_cast<uint64_t>(entryAddress[0]) |
                       (static_cast<uint64_t>(entryAddress[1]) << 32);
  OSSynchronizeIO();
  const uint64_t pteAfter = static_cast<uint64_t>(entryAddress[0]) |
                            (static_cast<uint64_t>(entryAddress[1]) << 32);
  OSSynchronizeIO();
  if (!page) { out.firstPTE = pte; out.nativeDescriptorFirstPhysical = expected; }
  if (pte != pteAfter) {
   out.code = ResultCode::GGTTEntryUnstable;
   out.mismatchPage = page;
   return out;
  }
  if (!(pte & 1)) {
   out.code = ResultCode::GGTTEntryNotPresent;
   out.mismatchPage = page;
   out.ggttMatchesDescriptor = Knowledge::No;
   return out;
  }
  if ((pte & kADLPTEAddressMask) != expected) {
   out.code = ResultCode::GGTTPhysicalMismatch;
   out.mismatchPage = page;
   out.ggttMatchesDescriptor = Knowledge::No;
   return out;
  }
  ++out.pagesChecked;
 }
 out.ggttMatchesDescriptor = Knowledge::Yes;
 // Retain prevents freeing, not redirect/rebinding. Refuse a mixed snapshot.
 uint64_t finalOffset=0;
 if(out.resourceType==0x82){uint32_t value=0;memcpy(&value,resourceBytes+0xfc,4);finalOffset=value;}
 else memcpy(&finalOffset,resourceBytes+0xf8,8);
 uint32_t finalMapFlags=0,finalMemoryFlags=0;
 memcpy(&finalMapFlags,mapBytes+0x10,4);memcpy(&finalMemoryFlags,memoryBytes+0xc,4);
 if(*reinterpret_cast<OSObject *const *>(resourceBytes+0x40)!=nativeMap||
    resourceBytes[0x14]!=out.resourceType||finalOffset!=out.resourceBackingOffset||
    finalMapFlags!=out.mapFlags||finalMemoryFlags!=out.memoryFlags||
    mapBytes[0x11c]||storageBytes[0x14]!=0xc0||
    *reinterpret_cast<OSObject *const *>(mapBytes+0x90)!=owner||
    *reinterpret_cast<OSObject *const *>(mapBytes+0x18)!=nativeMemory||
    *reinterpret_cast<IOMemoryDescriptor *const *>(memoryBytes+0xd0)!=nativeBacking||
    *reinterpret_cast<OSObject*const*>(resourceBytes+0x88)!=residentMemory||
    (residentMemory?residentMemory:*reinterpret_cast<OSObject*const*>(resourceBytes+0x80))!=nativeMemory||
    (out.resourceType==0x82&&*reinterpret_cast<OSObject*const*>(resourceBytes+0xe0)!=storage)||
    *reinterpret_cast<OSObject*const*>(storageBytes+0x80)!=surfaceMemory||
    *reinterpret_cast<IOMemoryDescriptor*const*>(surfaceMemoryBytes+0xd0)!=storageBacking||
    nativeBacking->getLength()!=out.nativeDescriptorLength||
    backing->getLength()!=out.surfaceDescriptorLength||
    storageBacking->getLength()!=out.storageDescriptorLength||
    ReimsIOAccelMapValue(nativeMap,0x128)!=out.nativeGPUVA||
    *reinterpret_cast<const uint64_t*>(mapBytes+0xc8)!=out.assignedLength||
    !(memoryBytes[0xc]&2)||
    ReimsIOAccelMapValue(nativeMap,0x168)!=out.nativeLength){
  out.ggttMatchesDescriptor=Knowledge::Unknown;out.physicalAlias=false;
  out.code=ResultCode::MappingChanged;return out;
 }
 out.code = ResultCode::Success;
 if(prepared){memcpy(prepared->identity,identity,sizeof(identity));prepared->result=out;prepared->valid=true;}
 return out;
}

} // namespace ReimsNativeMapping
