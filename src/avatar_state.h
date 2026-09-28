#pragma once

#include "guest_memory.h"

namespace sfr {
// The race manager owns the live racer list. Resolve it each frame: menu
// previews also query Avatar bodies, and cannot establish the selected rider.
inline uint32_t single_player_avatar_racer(const GuestMemory& memory) {
    constexpr uint32_t race_flag = 0x83E52F8C;
    constexpr uint32_t manager_global = 0x83E52FDC;
    constexpr uint32_t local_racers = 0x82B0569F;
    if (!memory.readable(race_flag, 4) || !memory.readable(manager_global, 4) ||
        !memory.readable(local_racers, 1) || !memory.load<uint32_t>(race_flag) ||
        memory.load<uint8_t>(local_racers) != 1) return 0;

    const uint32_t manager = memory.load<uint32_t>(manager_global);
    if (!manager || !memory.readable(manager, 44)) return 0;
    // +20 is the planned entrant count. Loading builds only the local preview
    // first (82289C20); 82289E00 populates the full list later. The vector's
    // initialized range, not completion of that plan, establishes the rider.
    const uint32_t planned_count = memory.load<uint32_t>(uint64_t(manager) + 20);
    const uint32_t begin = memory.load<uint32_t>(uint64_t(manager) + 36);
    const uint32_t end = memory.load<uint32_t>(uint64_t(manager) + 40);
    if (!planned_count || !begin || end <= begin || (end - begin) % 4 ||
        !memory.readable(begin, end - begin)) return 0;

    const uint32_t rider = memory.load<uint32_t>(begin);
    if (!rider || !memory.readable(rider, 108)) return 0;
    // +100 is the character; 17/18 at +104 are Avatar body/gear variants.
    return memory.load<uint32_t>(uint64_t(rider) + 100) == 17 ? rider : 0;
}
}
