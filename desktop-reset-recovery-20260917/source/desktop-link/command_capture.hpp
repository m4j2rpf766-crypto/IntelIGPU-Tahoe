#pragma once
#include "capture_walk.hpp"
// ADL-P 46a3 rev 0c only; called with native forcewake and no IOMapper.
// CPU-visible diagnostic samples, never a GPU completion or ownership proof.
namespace ReimsCommandCapture {
constexpr uint64_t mask=0x00003ffffffff000ULL;
inline void number(OSDictionary*d,const char*k,uint64_t v){auto*n=OSNumber::withNumber(v,64);if(n){d->setObject(k,n);n->release();}}
inline void data(OSDictionary*d,const char*k,const void*p,size_t n){auto*b=OSData::withBytes(p,n);if(b){d->setObject(k,b);b->release();}}
inline bool ggtt(IOMemoryMap*m,uint64_t va,uint64_t&pte){
 if(va>=0x100000000ULL)return false;
 uint32_t a=0,b=0,c=0;uint32_t off=0x800000+uint32_t((va>>12)*8);
 if(!read32(m,off+4,a)||!read32(m,off,b)||!read32(m,off+4,c)||a!=c)return false;
 pte=(uint64_t(a)<<32)|b;return (pte&1)!=0; // Native 16.0.3 read() 0x1072c; not a GPU-local-memory verdict.
}
// Reject large/64K/local-memory mappings rather than applying a 4K walk to them.
inline bool translate(IOMemoryMap*m,uint64_t root,uint64_t va,bool global,uint64_t&pa,uint64_t*entries,uint64_t*locations){
 if(global){if(!ggtt(m,va,entries[0]))return false;pa=entries[0]&0x7ffffff000ULL;return true;}
 if(va>>48)return false;
 uint64_t table=root&mask;const unsigned shifts[]={39,30,21,12};
 for(unsigned i=0;i<4;i++){
  locations[i]=table+((va>>shifts[i])&511)*8;
  if(!copyHighRAM(locations[i],entries+i,8)||!(entries[i]&1))return false;
  if(i<3&&(entries[i]&(0x80|0x800|0x40)))return false;
  if(i==3&&(entries[i]&(0x800|0x100)))return false;
  table=entries[i]&mask;
 }
 pa=table;return true;
}
inline OSDictionary* page(IOMemoryMap*m,uint64_t root,uint64_t va,bool global,uint8_t*buf){
 auto*d=OSDictionary::withCapacity(12);if(!d)return nullptr;
 number(d,"Address",va);number(d,"Global",global);number(d,"Root",root);
 number(d,"GPUMappingVerified",0);number(d,"NativeGGTTInterpretation",global);
 uint64_t pa=0,e[4]={},loc[4]={},pa2=0,e2[4]={},loc2[4]={};
 bool ok=translate(m,root,va,global,pa,e,loc);
 number(d,"Physical",pa);number(d,"NativePTEFlags",global?(e[0]&4095):0);data(d,"PTEs",e,sizeof(e));data(d,"PTELocations",loc,sizeof(loc));
 if(ok)ok=copyHighRAM(pa,buf,4096);
 if(ok)data(d,"Before",buf,4096);
 bool stable=ok&&translate(m,root,va,global,pa2,e2,loc2)&&pa==pa2&&!memcmp(e,e2,sizeof(e));
 bool reread=ok&&copyHighRAM(pa,buf+4096,4096);
 if(reread)data(d,"After",buf+4096,4096);
 stable=stable&&reread&&!memcmp(buf,buf+4096,4096);
 // Recheck translation after the second data read as well.
 stable=stable&&translate(m,root,va,global,pa2,e2,loc2)&&pa==pa2&&!memcmp(e,e2,sizeof(e));
 number(d,"ReadOK",ok);number(d,"RepeatedSampleEqual",stable);
 return d;
}
struct WalkReader {
 IOMemoryMap*map;uint64_t root;OSDictionary*pages;uint8_t*scratch;
 uint64_t addresses[24];bool global[24];OSData*contents[24];unsigned count,calls;
 uint64_t startNS;bool budgetHit;
};
inline uint64_t captureNS(){uint64_t t=0,n=0;clock_get_uptime(&t);absolutetime_to_nanoseconds(t,&n);return n;}
inline bool walkRead(void*opaque,uint64_t va,void*out,size_t bytes,bool global,ReimsCaptureWalk::Kind kind){
 auto&r=*static_cast<WalkReader*>(opaque);
 if(!ReimsCaptureWalk::range(va,bytes)||bytes>1024)return false;
 if(++r.calls>4096||captureNS()-r.startNS>150000000ULL){r.budgetHit=true;return false;}
 auto*dest=static_cast<uint8_t*>(out);
 while(bytes){
  const uint64_t base=va&~4095ULL;unsigned slot=0;
  for(;slot<r.count;slot++)if(r.addresses[slot]==base&&r.global[slot]==global)break;
  if(slot==r.count){
   if(r.count==24){r.budgetHit=true;return false;}
   r.addresses[slot]=base;r.global[slot]=global;r.count++;
   auto*v=page(r.map,r.root,base,global,r.scratch);if(!v)return false;
   number(v,"RequestedVA",va);number(v,"FirstUseKind",kind);number(v,"ExecutionProven",0);
   auto*equal=OSDynamicCast(OSNumber,v->getObject("RepeatedSampleEqual"));
   auto*body=OSDynamicCast(OSData,v->getObject("Before"));
   if(equal&&equal->unsigned64BitValue()&&body&&body->getLength()==4096)r.contents[slot]=body;
   char key[32];snprintf(key,sizeof(key),"FollowPage%u",slot);if(!r.pages->setObject(key,v)){r.contents[slot]=nullptr;r.budgetHit=true;}v->release();
  }
  if(!r.contents[slot])return false;
  size_t n=4096-(va&4095);if(n>bytes)n=bytes;
  memcpy(dest,static_cast<const uint8_t*>(r.contents[slot]->getBytesNoCopy())+(va&4095),n);
  dest+=n;va+=n;bytes-=n;
 }
 return true;
}
inline void followCommands(IOMemoryMap*m,OSDictionary*pages,OSDictionary*d,uint8_t*buf,uint64_t root,uint32_t ringStart,uint32_t size,uint32_t head){
 auto*r=static_cast<WalkReader*>(IOMalloc(sizeof(WalkReader)));
 auto*a=static_cast<ReimsCaptureWalk::Audit*>(IOMalloc(sizeof(ReimsCaptureWalk::Audit)));
 if(!r||!a){if(r)IOFree(r,sizeof(*r));if(a)IOFree(a,sizeof(*a));number(d,"WalkAllocationFailed",1);return;}
 bzero(r,sizeof(*r));bzero(a,sizeof(*a));r->map=m;r->root=root;r->pages=pages;r->scratch=buf;r->startNS=captureNS();
 // Seed only the batch-start immediately before ring HEAD, with circular reads.
 uint32_t jump[3]={};bool valid=true;
 for(unsigned i=0;i<3;i++)valid=walkRead(r,uint64_t(ringStart)+((head+size-12+i*4)%size),jump+i,4,true,ReimsCaptureWalk::Command)&&valid;
 if(valid&&(jump[0]&0xff8000ff)==0x18800001)ReimsCaptureWalk::queueCandidate(*a,(uint64_t(jump[2])<<32)|(jump[1]&~3U),!(jump[0]&256));
 else number(d,"WalkSeedUnavailable",1);
 ReimsCaptureWalk::run(*a,walkRead,r);
 data(d,"WalkEntries",a->entries,a->queued*sizeof(a->entries[0]));
 data(d,"InstructionBaseCandidates",a->bases,a->nbases*sizeof(uint64_t));
 data(d,"ShaderOffsetCandidates",a->offsets,a->noffsets*sizeof(uint64_t));
 number(d,"WalkPackets",a->packets);number(d,"WalkEntriesVisited",a->visited);number(d,"WalkDraws",a->draws);
 number(d,"WalkIndices",a->indices);number(d,"WalkVertices",a->vertices);number(d,"WalkShaders",a->shaders);
 number(d,"WalkRefused",a->refused);number(d,"WalkTruncated",a->truncated||r->budgetHit);
 number(d,"WalkPages",r->count);number(d,"WalkCalls",r->calls);number(d,"WalkElapsedNS",captureNS()-r->startNS);
 number(d,"WalkExecutionOrderProven",0);IOFree(a,sizeof(*a));IOFree(r,sizeof(*r));
}
inline void capture(IOService*s,IOMemoryMap*m){
 // Complete replacement prevents a failed new attempt from exposing stale pages.
 s->removeProperty("RCSCommandEvidenceV2");
 auto*d=OSDictionary::withCapacity(8);if(!d)return;
 const uint32_t offsets[]={0x2030,0x2034,0x2038,0x203c,0x205c,0x2060,0x2064,0x2068,0x206c,0x2070,0x2074,0x2078,0x20b0,0x20b8,0x20d0,0x2110,0x2114,0x2118,0x211c,0x2140,0x2168,0x2180,0x2234,0x2238,0x2270,0x2274,0x23a0,0x2510,0x2514,0x2518,0x251c,0x2520,0x2524,0x2528,0x252c,0x2530,0x2534,0x2538,0x253c,0x2540,0x2544,0x2548,0x254c};
 uint32_t before[sizeof(offsets)/4]={},after[sizeof(offsets)/4]={};bool ok=true;
 for(unsigned i=0;i<sizeof(offsets)/4;i++)ok=read32(m,offsets[i],before[i])&&ok;
 data(d,"Offsets",offsets,sizeof(offsets));data(d,"Before",before,sizeof(before));number(d,"Version",4);
 auto*pages=OSDictionary::withCapacity(36);auto*buf=static_cast<uint8_t*>(IOMalloc(8192));
 if(ok&&!(before[14]&3)&&pages&&buf){
  const uint64_t root=(uint64_t(before[25])<<32)|before[24];
  uint32_t size=(before[3]&0x1ff000)+4096;
  bool ring=(before[3]&1)&&before[2]&&!(before[2]&4095)&&size<=131072&&uint64_t(before[2])+size<=0x100000000ULL;
  number(d,"RingBytes",size);number(d,"RingInBounds",ring);
  if(ring)for(unsigned i=0;i<size/4096;i++){
   char key[32];snprintf(key,sizeof(key),"RingPage%u",i);
   auto*v=page(m,root,uint64_t(before[2])+i*4096,true,buf);
   if(v){pages->setObject(key,v);v->release();}
  }
  // Preserve at most four submitted descriptor candidates. A queued descriptor
  // is not proof of the current context; keep its state image for cross-checks.
  unsigned savedContexts=0;
  for(unsigned slot=0;slot<8&&savedContexts<4;slot++){
   const uint32_t low=before[27+slot*2];
   const uint64_t base=low&0xfffff000U;
   if(!(low&1)||!base||base>0xffffe000ULL)continue;
   auto*c=page(m,root,base+4096,true,buf);if(!c)continue;
   number(c,"DescriptorLow",low);number(c,"DescriptorHigh",before[28+slot*2]);
   number(c,"DescriptorSlot",slot);number(c,"CurrentContextProven",0);
   // Native initRingGPUVirtualAddress/control store at image +0x1024/+0x102c.
   // Use only fully read, repeated-equal bytes for these correlations.
   auto*valid=OSDynamicCast(OSNumber,c->getObject("ReadOK"));
   auto*equal=OSDynamicCast(OSNumber,c->getObject("RepeatedSampleEqual"));
   bool copied=valid&&valid->unsigned64BitValue()&&equal&&equal->unsigned64BitValue();
   uint32_t start=0,control=0;
   if(copied){memcpy(&start,buf+0x24,4);memcpy(&control,buf+0x2c,4);}
   number(c,"ContextRingStart",start);number(c,"ContextRingControl",control);
   number(c,"RingRegistersMatch",copied&&start==before[2]&&control==before[3]);
   char key[32];snprintf(key,sizeof(key),"ContextState%u",slot);pages->setObject(key,c);c->release();savedContexts++;
  }
  if(ring)followCommands(m,pages,d,buf,root,before[2],size,before[1]&0x1ffffc);
  const uint64_t addresses[]={(uint64_t(before[4])<<32)|before[10],(uint64_t(before[20])<<32)|before[19],(uint64_t(before[18])<<32)|before[16]};
  const char*names[]={"ActiveInstructionCandidate","BatchBufferCandidate","SecondBatchBufferCandidate"};
  for(unsigned i=0;i<3;i++)if(addresses[i]){
   // ACTHD address space follows the active batch only when it is active.
   bool global=i==2?!(before[17]&32):!(before[15]&32);
   if(i==0&&!addresses[1])continue;
   // ACTHD=0 leaves BBADDR potentially stale. Preserve it as a candidate only.
   auto*v=page(m,root,addresses[i],global,buf);
   if(v){pages->setObject(names[i],v);v->release();}
  }
 }
 if(buf)IOFree(buf,8192);
 if(pages){d->setObject("Pages",pages);pages->release();}
 bool same=ok;for(unsigned i=0;i<sizeof(offsets)/4;i++)same=read32(m,offsets[i],after[i])&&same;
 data(d,"After",after,sizeof(after));number(d,"RegistersReadOK",ok);number(d,"RegistersRepeatedEqual",same&&!memcmp(before,after,sizeof(before)));
 number(d,"AtomicOrOwnershipProof",0);s->setProperty("RCSCommandEvidenceV2",d);d->release();
}
}
