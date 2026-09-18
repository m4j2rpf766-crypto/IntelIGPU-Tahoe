#pragma once
#include <IOKit/IOLib.h>

// Ventura AGDC 0x711 consumed by IOPresentmentCapabilitiesDisplayPipe.
// This describes the implemented fixed BGRA plane, backed by the native Intel display pipe.
static inline IOReturn ReimsADLGraphicsControlPipelineCapabilities(
 unsigned long *input, unsigned long inputSize,
 unsigned long *output, unsigned long *outputSize) {
 constexpr unsigned long wireSize=0x196c; // 12-byte header + eight 0x32c entries
 if(!input||inputSize!=wireSize||!output||!outputSize||*outputSize<wireSize)
  return kIOReturnBadArgument;
 auto in=reinterpret_cast<const unsigned char*>(input);
 UInt32 connector=0,query=0,capacity=0;
 memcpy(&connector,in,4);memcpy(&query,in+4,4);memcpy(&capacity,in+8,4);
 if(connector>1||!capacity||capacity>8)return kIOReturnBadArgument;
 // IOP enumerates the sixteen single-bit query families below.
 // It initializes output count to capacity (8) before each call and consumes
 // some counts even after an error. A recognized but absent family must
 // therefore return an explicit empty collection, not leave capacity as count.
 // This follows native APV's successful optional-family queries, while using
 // count zero rather than publishing its inactive placeholder descriptors.
 if(!query || query>0x8000 || (query & (query-1)))return kIOReturnUnsupported;
 // Inline-buffer query 0x40 is a presence contract, unlike gamma enumeration.
 // IOP __GatherAGDCDisplayPipelineCapabilities (0x7ff8112ee79f) sets
 // supportsInlineBuffer@0x436 only for success with nonzero output size.
 // Success/count0 therefore emits an empty InlineBufferDictionary, which
 // __inlineBufferFromDictionary rejects as MALFORMED (0x2004). There is no
 // inline-buffer hardware here: Unsupported correctly omits that dictionary.
 bzero(output,wireSize);
 auto bytes=reinterpret_cast<unsigned char*>(output);
 const UInt32 count=query==0x10?1:0;
 memcpy(bytes,&connector,4);memcpy(bytes+4,&query,4);memcpy(bytes+8,&count,4);
 *outputSize=wireSize;
 if(query==0x40)return kIOReturnUnsupported;
 // No scaler, gamma, CSC, cursor, rotation or secondary plane support exists.
 // IOP CreateGammaDictionary returns no dictionary when the gamma count is 0.
 if(!count)return kIOReturnSuccess;
 // One primary format descriptor. Native consumer at 0x7ff8112f16ad reads
 // u64@40 count, ORs u32@64 for each 32-byte descriptor, converts AGDC 0x10000
 // to IOP 1<<48 and then FourCC BGRA (0x42475241). No blending is performed.
 const UInt64 formatCount=1;
 const UInt32 bgra=0x10000;
 memcpy(bytes+40,&formatCount,8);memcpy(bytes+64,&bgra,4);
 // Flags@12, blend@16 and alternate-format count@432 remain zero.
 *outputSize=wireSize;
 return kIOReturnSuccess;
}
