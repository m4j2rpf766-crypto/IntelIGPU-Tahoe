#pragma once
#include <stdint.h>
namespace ReimsPWM {
inline bool valid(uint32_t ctl,uint32_t period,uint32_t duty,uint32_t ctl1){
 return ctl==0x80000000U&&period&&duty<=period&&!(ctl1&0x80000000U);
}
inline uint32_t toDuty(uint32_t level,uint32_t period){return uint32_t((uint64_t(level)*period+127)/255);}
inline uint32_t toUser(uint32_t duty,uint32_t period){return period?uint32_t((uint64_t(duty)*255+period/2)/period):0;}
// Linear PWM fraction, distinct from the 8-bit legacy user parameter.
constexpr uint32_t linearMax=65535;
inline uint32_t linearToDuty(uint32_t level,uint32_t period){return uint32_t((uint64_t(level)*period+linearMax/2)/linearMax);}
inline uint32_t dutyToLinear(uint32_t duty,uint32_t period){return period?uint32_t((uint64_t(duty)*linearMax+period/2)/period):0;}
inline uint32_t userToLinear(uint32_t level){return level*257;}
inline uint32_t linearToUser(uint32_t level){return (level+128)/257;}
// User policy: even slider zero must leave the lit panel at >=1% PWM.
// Round upward so periods not divisible by 100 cannot undershoot the floor.
inline uint32_t minimumPanelDuty(uint32_t period){return uint32_t((uint64_t(period)+99)/100);}
inline uint32_t panelDuty(uint32_t level,uint32_t period){
 const auto requested=linearToDuty(level,period),minimum=minimumPanelDuty(period);
 return requested<minimum?minimum:requested;
}
}
