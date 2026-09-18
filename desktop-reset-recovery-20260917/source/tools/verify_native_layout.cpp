#include "../desktop-link/native_layout.hpp"
#include <cassert>
#include <cstdio>
using namespace ReimsNativeLayout;
int main(){
 Input captured;
 captured.width=1920;captured.height=1080;captured.surfacePitch=7680;
 captured.fourcc=0x42475241;captured.resourceFormat=0xc0;captured.tileMode=3;
 captured.rowA=captured.rowB=7680;captured.backingLength=0x7f8000;captured.storageKnown=true;
 auto p=describe(captured);
 assert(p.status==Status::OK&&p.planeStride==60&&p.planeTilingBits==0x1000);
 assert(p.storageHeight==1088&&p.requiredBytes==0x7f8000);
 assert(baseSuitable(p,0x100000)&&!baseSuitable(p,0x101000));
 assert(!baseSuitable(p,0x100000000ULL)&&!baseSuitable(p,0xfff00000));
 // All visible BGRA pixels remain within the padded
 // Y allocation, including the final partial tile row. Closed-form Intel
 // Y addresses are independent of the layout planner's size arithmetic.
 uint64_t last=0;
 for(uint32_t y=0;y<1080;++y)for(uint32_t x=0;x<1920;++x){
  const uint64_t xb=uint64_t(x)*4;
  const uint64_t a=(y/32)*uint64_t(7680)*32+(xb/128)*4096+
                   ((xb%128)/16)*512+(y%32)*16+xb%16;
  assert(a+4<=p.requiredBytes);if(a>last)last=a;
 }
 assert(last+4>uint64_t(7680)*1080); // catches the old unpadded bound
 auto i=captured;i.backingLength=uint64_t(7680)*1080;assert(describe(i).status==Status::BackingTooShort);
 i=captured;i.backingLength--;assert(describe(i).status==Status::BackingTooShort);
 i=captured;i.rowA=0;assert(describe(i).status==Status::OK);
 i=captured;i.rowA=UINT64_MAX;assert(describe(i).status==Status::PitchMismatch);
 i=captured;i.rowA=i.surfacePitch=7700;assert(describe(i).status==Status::PitchAlignment);
 i=captured;i.rowA=i.surfacePitch=128*4096;assert(describe(i).status==Status::StrideOverflow);
 i=captured;i.clientOffset=UINT64_MAX;i.planeOffset=1;assert(describe(i).status==Status::OffsetUnsupported);
 i=captured;i.compressed=true;assert(describe(i).status==Status::CompressionUnsupported);
 i=captured;i.storageKnown=false;assert(describe(i).status==Status::StorageUnknown);
 i=captured;i.tileMode=7;assert(describe(i).status==Status::UnsupportedTileMode);
 i=captured;i.planeCount=2;assert(describe(i).status==Status::UnsupportedPlanes);
 i=captured;i.fourcc=0x34323076;assert(describe(i).status==Status::UnsupportedFormat);
 i=captured;i.resourceFormat=0xd1;assert(describe(i).status==Status::UnsupportedFormat);
 i=captured;i.width=1280;assert(describe(i).status==Status::BadGeometry);
 i=captured;i.tileMode=0;p=describe(i);
 assert(p.status==Status::OK&&p.planeStride==120&&p.planeTilingBits==0&&p.storageHeight==1080);
 i=captured;i.tileMode=2;p=describe(i);
 assert(p.status==Status::OK&&p.planeStride==15&&p.planeTilingBits==0x400&&p.storageHeight==1080);
 puts("PASS: captured Y target; linear/X/Y stride; padded last row; invalid formats, compression, offsets, bounds and GGTT alignment");
}
