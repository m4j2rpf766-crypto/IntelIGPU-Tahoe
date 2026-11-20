#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace reims {

// The current IOAccelVideoContext stores these four ranges in its client
// context. They are read-only identity checks, not a substitute for the
// kernel's resource and fence ownership.
inline bool video_buffers_match_context(const uint64_t *context, size_t words,
                                        void *shared, void *io,
                                        const void *commands, size_t command_bytes,
                                        const void *sideband, size_t sideband_bytes) {
    constexpr size_t kCommandLimit = 1024 * 1024;
    constexpr size_t kSidebandLimit = 64 * 1024;
    if (!context || words < 17 || !shared || !io || !commands || !sideband ||
        !command_bytes || !sideband_bytes ||
        command_bytes > kCommandLimit || sideband_bytes > kSidebandLimit ||
        (command_bytes & 3) || (sideband_bytes & 3)) return false;

    const uintptr_t command_start = reinterpret_cast<uintptr_t>(commands);
    const uintptr_t sideband_start = reinterpret_cast<uintptr_t>(sideband);
    if (command_start > std::numeric_limits<uintptr_t>::max() - command_bytes ||
        sideband_start > std::numeric_limits<uintptr_t>::max() - sideband_bytes)
        return false;

    const uintptr_t command_end = command_start + command_bytes;
    const uintptr_t sideband_end = sideband_start + sideband_bytes;
    if (command_start < sideband_end && sideband_start < command_end) return false;

    return context[0] == reinterpret_cast<uintptr_t>(shared) &&
           context[1] == reinterpret_cast<uintptr_t>(io) &&
           context[2] == command_start && context[4] == command_end &&
           context[16] == sideband_start && context[12] == sideband_end;
}

// Confirm that the declared command extent is the one carried by the live
// sideband and that the allocation tail is not mistaken for submitted data.
// The packet rewrite validates command encodings; this is the final live-range
// boundary check immediately before IOAccelVideoContextSubmitDataBuffers.
inline bool video_batch_used_boundary_matches(const uint32_t *commands,
                                               size_t command_dwords,
                                               uint32_t used_dwords,
                                               const uint32_t *sideband,
                                               size_t sideband_dwords) {
    if (!commands || !sideband || !used_dwords || used_dwords > command_dwords ||
        sideband_dwords < 9 || commands[used_dwords - 1] != 0x05000000u)
        return false;

    for (size_t i = used_dwords; i < command_dwords; ++i)
        if (commands[i] != 0) return false;

    bool used_token = false;
    bool terminal = false;
    for (size_t t = 4; t < sideband_dwords;) {
        const uint32_t token = sideband[t];
        if (token == 0x200u) {
            terminal = t + 2 == sideband_dwords;
            break;
        }
        const size_t packet_dwords = token >> 16;
        if (packet_dwords < 2 || packet_dwords > sideband_dwords - t)
            return false;
        if (token == 0x38000u) {
            if (packet_dwords != 3 || used_token || sideband[t + 1] != used_dwords)
                return false;
            used_token = true;
        }
        t += packet_dwords;
    }
    return used_token && terminal;
}

}  // namespace reims
