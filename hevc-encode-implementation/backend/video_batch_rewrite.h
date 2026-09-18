#pragma once
#include <cstdint>
#include <utility>
#include <vector>
namespace reims {
struct PacketReplacement {
    uint32_t old_dword, old_length;
    std::vector<uint32_t> words;
    // Explicit mapping only for address DWORDs in a replaced packet.
    std::vector<std::pair<uint32_t,uint32_t>> address_dwords;
    std::vector<uint32_t> before;
};
struct VideoBatch {
    std::vector<uint32_t> commands, sideband;
    uint32_t used_dwords=0;
};
// Rewrite within the existing allocation. Resource handles, addends, access
// modes, fences and status commands remain intact. This validates relocation
// structure, NOT GPU compatibility of the replacement command contents.
// Invalid inputs leave output unchanged.
bool rewrite_video_batch(const VideoBatch &, const std::vector<PacketReplacement> &, VideoBatch &);
}
