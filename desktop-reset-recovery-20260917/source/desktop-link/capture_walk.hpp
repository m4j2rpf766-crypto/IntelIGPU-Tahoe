#pragma once
#include <stdint.h>
#include <stddef.h>
// Bounded diagnostic candidate traversal, NOT an execution emulator. Field
// layouts: archived Mesa gen120/gen110 XML in incident-v3-214127-analysis/genxml.
namespace ReimsCaptureWalk {
enum Kind {Command=1,Index=2,Vertex=3,Shader=4};
typedef bool (*Reader)(void*,uint64_t,void*,size_t,bool,Kind);
struct Entry{uint64_t va;bool global;};
struct VB{uint64_t base;uint32_t size,pitch;bool valid;};
struct Audit {
 Entry entries[8];unsigned queued,visited,packets,draws,indices,vertices,shaders,refused;
 uint64_t bases[4],offsets[12];unsigned nbases,noffsets;
 bool truncated,executionProven;
};
inline bool range(uint64_t a,size_t n){return n && a<0x1000000000000ULL && n<=0x1000000000000ULL-a;}
inline void queueCandidate(Audit&a,uint64_t va,bool global){
 if(!range(va,4)||(va&3)){a.refused++;return;}
 for(unsigned i=0;i<a.queued;i++)if(a.entries[i].va==va&&a.entries[i].global==global)return;
 if(a.queued==8){a.truncated=true;return;}a.entries[a.queued++]={va,global};
}
inline void unique(uint64_t*v,unsigned&n,unsigned cap,uint64_t x,Audit&a){for(unsigned i=0;i<n;i++)if(v[i]==x)return;if(n<cap)v[n++]=x;else a.truncated=true;}
inline void run(Audit&a,Reader read,void*ctx){
 for(unsigned q=0;q<a.queued;q++){
  a.visited++;uint64_t pc=a.entries[q].va;bool global=a.entries[q].global;
  VB vb[32]={};uint64_t ib=0;uint32_t ibsize=0,format=3;bool haveIB=false,instanced=false;
  for(unsigned step=0;step<256;step++){
   if(a.packets>=512){a.truncated=true;break;}
   uint32_t w[256]={};if(!read(ctx,pc,w,4,global,Command))break;
   const uint32_t h=w[0],type=h>>29,op=(h>>23)&63;unsigned n=0;
   if(h==0||(type==0&&(op==2||op==5||op==7||op==10)))n=1;
   else if((h&0xffff0000)==0x69040000||(h&0xffff0000)==0x680b0000)n=1;
   else if(type==3||type==0)n=(h&255)+2;
   else {a.refused++;break;}
   if(n>256||!range(pc,n*4)||!read(ctx,pc,w,n*4,global,Command)){a.refused++;break;}
   a.packets++;
   if(type==0&&op==10)break;
   if((h&0xff8000ff)==0x18800001&&n==3)queueCandidate(a,(uint64_t(w[2])<<32)|(w[1]&~3U),!(h&256));
   // Fall-through is scanned as another candidate path, never asserted executed.
   if((h&0xffff0000)==0x61010000&&n>=16&&(w[10]&1))unique(a.bases,a.nbases,4,((uint64_t(w[11])<<32)|w[10])&~4095ULL,a);
   if((h&0xffff0000)==0x78100000&&n==9)unique(a.offsets,a.noffsets,12,((uint64_t(w[2])<<32)|w[1])&~63ULL,a);
   if((h&0xffff0000)==0x78200000&&n==12){const unsigned slots[]={1,8,10};for(unsigned k:slots)unique(a.offsets,a.noffsets,12,((uint64_t(w[k+1])<<32)|w[k])&~63ULL,a);}
   if((h&0xffff0000)==0x78080000&&(n-1)%4==0)for(unsigned k=1;k+3<n;k+=4){unsigned slot=w[k]>>26;vb[slot]={ (uint64_t(w[k+2])<<32)|w[k+1],w[k+3],w[k]&4095,(w[k]&(1<<13))==0&&(w[k]&(1<<14))!=0};}
   if((h&0xffff0000)==0x780a0000&&n==5){ib=(uint64_t(w[3])<<32)|w[2];ibsize=w[4];format=(w[1]>>8)&3;haveIB=true;}
   if((h&0xffff0000)==0x78490000&&n==3&&(w[1]&256))instanced=true;
   if((h&0xffff0000)==0x7b000000&&n>=7){
    a.draws++;
    // Only direct, single-instance indexed draws with base vertex zero. Other
    // modes need additional state: record refusal rather than guessing ranges.
    if(instanced||!haveIB||format>2||(h&0x500)||!(w[1]&256)||w[2]>64||!w[2]||w[4]!=1||w[5]||w[6]){a.refused++;}
    else {
     unsigned width=1U<<format;uint64_t start=uint64_t(w[3])*width;size_t bytes=size_t(w[2])*width;uint8_t values[256]={};
     if(start>ibsize||bytes>ibsize-start||!range(ib,start+bytes)||!read(ctx,ib+start,values,bytes,false,Index)){a.refused++;}
     else {a.indices+=w[2];for(unsigned i=0;i<w[2];i++){
      uint32_t index=0;for(unsigned j=0;j<width;j++)index|=uint32_t(values[i*width+j])<<(8*j);
      for(unsigned slot=0;slot<32;slot++)if(vb[slot].valid){
       uint64_t offset=uint64_t(index)*vb[slot].pitch;unsigned count=vb[slot].pitch;
       // Entire stride is bounded; per-instance modes remain candidate reads.
       if(!count||count>256||offset>vb[slot].size||count>vb[slot].size-offset||!range(vb[slot].base,offset+count)){a.refused++;continue;}
       uint8_t vertex[256];if(read(ctx,vb[slot].base+offset,vertex,count,false,Vertex))a.vertices++;else a.refused++;
      }
     }}
    }
   }
   pc+=n*4;
   if(step==255)a.truncated=true;
  }
 }
 for(unsigned b=0;b<a.nbases;b++)for(unsigned o=0;o<a.noffsets;o++){
  if(a.offsets[o]>=0x1000000000000ULL-64||!range(a.bases[b],a.offsets[o]+64)){a.refused++;continue;}
  uint8_t code[64];if(read(ctx,a.bases[b]+a.offsets[o],code,sizeof(code),false,Shader))a.shaders++;else a.refused++;
 }
}
}
