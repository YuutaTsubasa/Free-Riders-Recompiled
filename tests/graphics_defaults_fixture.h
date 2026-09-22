#pragma once
#include "guest_memory.h"
#include <array>

namespace sfr::test {
inline void install_sampler_defaults(GuestMemory& memory) {
    constexpr std::array<std::array<uint32_t, 2>, 20> rows{{
        {0x824E89C0,0}, {0x824E8A10,0}, {0x824E8A60,0}, {0x824E8950,0},
        {0x824E83F0,0}, {0x824E8248,0}, {0x824E8590,2}, {0x824E87B0,0},
        {0x824E8850,0}, {0x824E8690,1}, {0x824E84F0,0}, {0x824E8348,0},
        {0x824E85E8,0}, {0x824E88D0,13}, {0x824E8AB0,0}, {0x824E8708,0},
        {0x824E8B08,0}, {0x824E8B60,0}, {0x824E8BB8,0}, {0x824E8C10,1}
    }};
    for (uint32_t i = 0; i < rows.size(); ++i) {
        memory.store<uint32_t>(0x82AD0F20 + i * 12 + 4, rows[i][0]);
        memory.store<uint32_t>(0x82AD0F20 + i * 12 + 8, rows[i][1]);
    }
    if (memory.available(0x82001000, 0x1000)) memory.map(0x82001000, 0x1000);
    constexpr uint32_t lookup[]{0,0,2,2,3,3,3,4,4,4,4,4,4,5,5,5,5};
    for (uint32_t i = 0; i < std::size(lookup); ++i)
        memory.store<uint32_t>(0x82001608 + 4 * i, lookup[i]);
}
// Independent fixture values from original 82AD0A60, not production constants.
inline void install_blend_defaults(GuestMemory& memory) {
    constexpr std::array<std::array<uint32_t, 3>, 11> rows{{
        {0x3C,0x824E6A40,0}, {0x40,0x824E6DD0,0},
        {0x48,0x824E6B60,1}, {0x4C,0x824E6BF0,0},
        {0x50,0x824E6AD0,0}, {0x54,0x824E6CF0,1},
        {0x58,0x824E6D60,0}, {0x5C,0x824E6C80,0},
        {0x17C,0x824E8228,0}, {0x180,0x824E80D0,2}, {0x184,0x824E8100,2}
    }};
    for (auto row : rows) {
        memory.store<uint32_t>(0x82AD0A60 + row[0] * 3 + 4, row[1]);
        memory.store<uint32_t>(0x82AD0A60 + row[0] * 3 + 8, row[2]);
    }
}
}
