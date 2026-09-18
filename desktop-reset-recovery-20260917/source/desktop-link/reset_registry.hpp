#pragma once
#include <stddef.h>
#include <stdint.h>
// Pinned TGL16.0.3: native.asm resetGraphicsEngine 0x43716..0x43861.
// Entries are saved BEFORE reset, restored under native forcewake BEFORE
// scheduler processGPUReset. Native owns the allocation after installation.
struct ReimsResetEntry { uint32_t reg,value; uint8_t masked,context; char name[24]; uint8_t pad[2]; };
struct ReimsResetVector { uint64_t count,capacity; ReimsResetEntry *entries; };
static_assert(sizeof(ReimsResetEntry)==36 && offsetof(ReimsResetEntry,name)==10,"entry ABI");
static_assert(sizeof(ReimsResetVector)==24 && offsetof(ReimsResetVector,entries)==16,"vector ABI");
extern "C" ReimsResetVector rcsResetRegisters;
static inline bool reimsResetProfile(const ReimsResetVector &v) {
 if(!v.entries || (v.count!=32 && v.count!=33) || v.capacity<v.count || v.capacity>64)return false;
 const uint32_t regs[32]={0x2080,0x2134,0x20c0,0x7000,0x7004,0x20a8,0x209c,0x2090,0x4090,0x229c,0x20a0,0x20e4,0x9430,0x7010,0xd08,0xe194,0xb004,0x20ec,0x2580,0x2058,0x20e0,0x20d4,0x24d0,0x24d4,0x24d8,0x24dc,0x24e0,0x24ec,0x24f0,0x24f4,0x24f8,0x24fc};
 const uint8_t flags[32]={0,0,1,1,1,2,1,1,0,1,0,1,0,1,0,1,0,1,1,2,1,3,2,2,2,2,2,2,2,2,2,2};
 for(unsigned i=0;i<32;i++)if(v.entries[i].reg!=regs[i] ||
   v.entries[i].masked!=(flags[i]&1) || v.entries[i].context!=(flags[i]>>1))return false;
 if(v.count==33){const auto&e=v.entries[32];return e.reg==0x2050 && e.masked==1 && e.context==0;}
 return true;
}
