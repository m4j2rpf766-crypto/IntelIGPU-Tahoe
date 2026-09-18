#pragma once
#include <stdint.h>

// One armed update per native pipe: itcm has no transaction/stamp argument.
// SURFLIVE is hardware state; a posting read of SURF is not completion.
namespace ReimsFlip {
struct State {
 uint64_t submitted=0,latched=0,completed=0,armWrap=0;
 uint32_t base=0;
 bool pending=false,enabled=false;
 bool arm(uint32_t address,bool enable,uint64_t wraps){
  if(pending)return false;
  base=address;enabled=enable;armWrap=wraps;pending=true;++submitted;
  return true;
 }
 bool observe(uint32_t live,uint64_t wraps){
  if(!pending||latched==submitted||wraps<=armWrap)return false;
  if((live&0xfffff000U)!=base)return false;
  latched=submitted;return true;
 }
 bool finish(){
  if(!pending||latched!=submitted)return false;
  completed=submitted;pending=false;return true;
 }
 bool complete()const{return submitted&&completed==submitted&&!pending;}
};
}
