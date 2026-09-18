#include "pwm.hpp"
#include <initializer_list>
#include <cassert>
#include <cstdio>
int main(){
 assert(ReimsPWM::valid(0x80000000,48000,15058,0));
 assert(!ReimsPWM::valid(0,48000,15058,0));
 assert(!ReimsPWM::valid(0x80000000,0,0,0));
 assert(!ReimsPWM::valid(0x80000000,48000,48001,0));
 assert(!ReimsPWM::valid(0x80000000,48000,15058,0x80000000));
 for(uint32_t period:{48000U,65535U,0xffffffffU}){
  uint32_t prior=0;
  for(uint32_t v=0;v<=255;v++){
   auto duty=ReimsPWM::toDuty(v,period);
   assert(duty>=prior&&duty<=period);assert(ReimsPWM::toUser(duty,period)==v);prior=duty;
  }
  assert(ReimsPWM::toDuty(0,period)==0);assert(ReimsPWM::toDuty(255,period)==period);
  for(uint32_t v=0;v<=255;v++){
   assert(ReimsPWM::linearToUser(ReimsPWM::userToLinear(v))==v);
   assert(ReimsPWM::linearToDuty(ReimsPWM::userToLinear(v),period)==ReimsPWM::toDuty(v,period));
  }
  prior=0;
  for(uint32_t v=0;v<=ReimsPWM::linearMax;v++){
   const auto duty=ReimsPWM::linearToDuty(v,period);
   assert(duty>=prior&&duty<=period);prior=duty;
   const auto roundtrip=ReimsPWM::dutyToLinear(duty,period);
   assert(roundtrip+1>=v&&roundtrip<=v+1);
  }
  assert(ReimsPWM::linearToDuty(ReimsPWM::linearMax,period)==period);
 }
 for(uint32_t period:{1U,99U,101U,48000U,65535U,0xffffffffU}){
  const auto floor=ReimsPWM::minimumPanelDuty(period);
  assert(uint64_t(floor)*100>=period);
  assert(uint64_t(floor-1)*100<period);
  assert(ReimsPWM::panelDuty(0,period)==floor);
  uint32_t prior=0;
  for(uint32_t v=0;v<=ReimsPWM::linearMax;v++){
   const auto duty=ReimsPWM::panelDuty(v,period),raw=ReimsPWM::linearToDuty(v,period);
   assert(duty>=floor&&duty<=period&&duty>=prior);prior=duty;
   if(raw>=floor)assert(duty==raw);
  }
  assert(ReimsPWM::panelDuty(ReimsPWM::linearMax,period)==period);
 }
 assert(ReimsPWM::panelDuty(0,48000)==480);
 puts("PWM conversion: 3 periods, all legacy and 16-bit linear levels pass; invalid configurations rejected");
 puts("Panel floor: all 16-bit values at six periods >=1%; full brightness preserved");
}
