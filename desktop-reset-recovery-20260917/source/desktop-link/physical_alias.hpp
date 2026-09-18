#pragma once
#include <stdint.h>
namespace ReimsPhysicalAlias {
struct Segment {uint64_t address,length;};
enum class Status {Equal, InvalidRange, MissingSegment, DifferentPages};
struct Proof {
 Status status=Status::InvalidRange;
 uint64_t pages=0,mismatch=~0ULL,firstA=0,firstB=0;
};
// Read functions receive descriptor-relative offsets. A subrange descriptor's
// getPhysicalSegment already translates its own parent offset.
template<class A,class B> Proof compare(uint64_t size,A readA,B readB){
 Proof p;if(!size||size>UINT64_MAX-4095)return p;
 const uint64_t count=(size+4095)/4096;
 for(uint64_t page=0;page<count;++page){
  const uint64_t offset=page*4096,need=size-offset<4096?size-offset:4096;
  const Segment a=readA(offset),b=readB(offset);
  if(!page){p.firstA=a.address;p.firstB=b.address;}
  if(!a.address||!b.address||(a.address&4095)||(b.address&4095)||a.length<need||b.length<need){
   p.status=Status::MissingSegment;p.mismatch=page;return p;
  }
  if(a.address!=b.address){p.status=Status::DifferentPages;p.mismatch=page;return p;}
  ++p.pages;
 }
 p.status=Status::Equal;return p;
}
}
