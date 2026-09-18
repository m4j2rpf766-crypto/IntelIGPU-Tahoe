#pragma once
#include <stdint.h>
namespace ReimsFlipProfile {
// All nanoseconds use kernel uptime. A record links the native render-ready
// flip request to the actual hardware latch and retirement; this is NOT GPU
// execution time. Metal GPU timestamps are captured separately in userspace.
struct Record {
 uint64_t request=0,locked=0,layout=0,pinned=0,mapped=0,diagnostics=0;
 uint64_t armed=0,latched=0,completed=0,released=0,returned=0;
 uint64_t frame=0,base=0,cached=0;
 uint64_t surfaceID=0;
};
constexpr unsigned capacity=4096; // diagnostic capacity, never limits rendering
}
