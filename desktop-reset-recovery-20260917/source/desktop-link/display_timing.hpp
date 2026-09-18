#pragma once
#include <stdint.h>

// ADL-P Display 13, transcoder A -> combo PHY A, DP SST. Read-only
// reconstruction of the running mode, not a table of advertised frequencies.
// Register/PLL formulas: Linux i915 intel_display_regs.h/intel_dpll_mgr.c.
namespace ReimsDisplayTiming {
static constexpr uint32_t offsets[]={
 0x60000,0x60004,0x60008,0x6000c,0x60010,0x60014,0x6001c,0x6007c,
 0x60030,0x60034,0x60040,0x60044,0x60400,0x46140,0x164280,0x51004,
 0x46010,0x46014,0x164284,0x164288,0x16428c,0x164290,
 0x60420,0x6042c,0x60424,0x60434,0x70008,0x70040,0x70000,0x70048,0x44070};
static constexpr unsigned count=sizeof(offsets)/sizeof(offsets[0]);
struct Snapshot {
 uint32_t value[count]={};
 uint32_t get(uint32_t address)const{
  for(unsigned i=0;i<count;++i)if(offsets[i]==address)return value[i];
  return 0;
 }
};
struct Mode {
 uint32_t hActive=0,hTotal=0,hSyncStart=0,hSyncEnd=0;
 uint32_t vActive=0,vTotal=0,vSyncStart=0,vSyncEnd=0;
 uint32_t refresh1616=0,pll=0,lanes=0,bpc=0;
 uint64_t portClockHz=0,pixelClockHz=0;
 bool hPositive=false,vPositive=false;
};
enum class Status:uint32_t {OK,Route,Disabled,VariableRefresh,PLL,Geometry,Clock,Link};
inline Status decode(const Snapshot&s,Mode&m){
 m={};const uint32_t ddi=s.get(0x60400),mux=s.get(0x164280);
 if(((ddi>>27)&15)!=1||((ddi>>24)&7)!=2||
    (s.get(0x46140)&0xf0000000U)!=0x10000000U)return Status::Route;
 if(!(ddi&0x80000000U)||!(s.get(0x70008)&0x40000000U)||(mux&(1U<<10)))return Status::Disabled;
 // Reject VRR and interlace instead of presenting an invented fixed mode.
 if((s.get(0x60420)&0x80000000U)||(s.get(0x6042c)&(1U<<27)))return Status::VariableRefresh;
 if(s.get(0x70008)&(7U<<21))return Status::Geometry;
 m.pll=mux&3;if(m.pll>1)return Status::PLL;
 const uint32_t enable=s.get(m.pll?0x46014:0x46010);
 if((enable&0xc0000000U)!=0xc0000000U)return Status::PLL;
 const uint32_t refCode=s.get(0x51004)>>29;
 if(refCode>2)return Status::Clock;
 const uint64_t ref=refCode?19200000:24000000;
 const uint32_t c0=s.get(m.pll?0x16428c:0x164284),c1=s.get(m.pll?0x164290:0x164288);
 const unsigned pCode=(c1>>2)&15,kCode=(c1>>6)&7;
 const unsigned p=pCode==1?2:pCode==2?3:pCode==4?5:pCode==8?7:0;
 const unsigned k=kCode==1?1:kCode==2?2:kCode==4?3:0;
 const unsigned q=(c1&(1U<<9))?((c1>>10)&255):1;
 if(!p||!k||!q)return Status::PLL;
 // Display WA #22010492432: at 38.4MHz ADLP stores half the fraction.
 const uint64_t fraction=((c0>>10)&0x7fff)*(refCode==2?2U:1U);
 const uint64_t dco32768=(uint64_t(c0&1023)*32768+fraction)*ref;
 m.portClockHz=(dco32768+uint64_t(p)*q*k*5*16384)/(uint64_t(p)*q*k*5*32768);
 const uint32_t lm=s.get(0x60040)&0xffffff,ln=s.get(0x60044)&0xffffff;
 if(!lm||!ln||!m.portClockHz||m.portClockHz>810000000)return Status::Clock;
 m.pixelClockHz=(m.portClockHz*lm+ln/2)/ln;
 const uint32_t h=s.get(0x60000),v=s.get(0x6000c),hs=s.get(0x60008),vs=s.get(0x60014);
 m.hActive=(h&65535)+1;m.hTotal=(h>>16)+1;
 m.vActive=(v&65535)+1;m.vTotal=(v>>16)+1;
 m.hSyncStart=(hs&65535)+1;m.hSyncEnd=(hs>>16)+1;
 m.vSyncStart=(vs&65535)+1;m.vSyncEnd=(vs>>16)+1;
 if(m.hTotal<=m.hActive||m.vTotal<=m.vActive||m.hSyncStart<m.hActive||
    m.hSyncEnd<=m.hSyncStart||m.hSyncEnd>m.hTotal||m.vSyncStart<m.vActive||
    m.vSyncEnd<=m.vSyncStart||m.vSyncEnd>m.vTotal)return Status::Geometry;
 const uint64_t total=uint64_t(m.hTotal)*m.vTotal;
 const uint64_t refresh=(m.pixelClockHz*65536+total/2)/total;
 if(!refresh||refresh>0xffffffffULL)return Status::Clock;
 m.refresh1616=uint32_t(refresh);
 const unsigned bpcCode=(ddi>>20)&7;
 if(bpcCode>3)return Status::Link;
 m.bpc=bpcCode==0?8:bpcCode==1?10:bpcCode==2?6:12;
 m.lanes=((ddi>>1)&7)+1;
 if(m.lanes!=1&&m.lanes!=2&&m.lanes!=4)return Status::Link;
 if(m.pixelClockHz*3*m.bpc>m.portClockHz*8*m.lanes)return Status::Link;
 m.hPositive=ddi&(1U<<16);m.vPositive=ddi&(1U<<17);
 return Status::OK;
}
}
