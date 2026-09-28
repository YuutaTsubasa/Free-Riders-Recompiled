#pragma once

#include "guest_memory.h"
#include <algorithm>
#include <optional>

namespace sfr {
struct LocalAvatar {
    uint32_t manager = 0, rider = 0, character = 0;
    uint32_t renderer = 0, controller = 0, animation = 0;
    uint32_t local_slot = 0, local_count = 0;
};

namespace avatar_state_detail {
struct LocalRacers {
    uint32_t manager, begin, populated_count, local_count;
};

// The race manager owns the live racer list. Resolve it each frame: menu
// previews also query Avatar bodies, and cannot establish the selected rider.
inline std::optional<LocalRacers> local_racers(const GuestMemory& memory) {
    constexpr uint32_t race_flag = 0x83E52F8C;
    constexpr uint32_t manager_global = 0x83E52FDC;
    constexpr uint32_t local_count_address = 0x82B0569F;
    if (!memory.readable(race_flag, 4) || !memory.readable(manager_global, 4) ||
        !memory.readable(local_count_address, 1) || !memory.load<uint32_t>(race_flag)) return std::nullopt;
    const uint32_t local_count = memory.load<uint8_t>(local_count_address);
    if (local_count < 1 || local_count > 2) return std::nullopt;

    const uint32_t manager = memory.load<uint32_t>(manager_global);
    if (!manager || !memory.readable(manager, 44)) return std::nullopt;
    // +20 is the planned entrant count. Loading builds only the local preview
    // first (82289C20); 82289E00 populates the full list later. The vector's
    // initialized range, not completion of that plan, establishes the rider.
    const uint32_t planned_count = memory.load<uint32_t>(uint64_t(manager) + 20);
    const uint32_t begin = memory.load<uint32_t>(uint64_t(manager) + 36);
    const uint32_t end = memory.load<uint32_t>(uint64_t(manager) + 40);
    if (!planned_count || !begin || end <= begin || (end - begin) % 4 ||
        (end - begin) / 4 > planned_count || !memory.readable(begin, end - begin)) return std::nullopt;
    return LocalRacers{manager, begin, (end - begin) / 4, local_count};
}

template<class Match>
inline std::optional<LocalAvatar> find(const GuestMemory& memory, Match match) {
    const auto racers = local_racers(memory);
    if (!racers) return std::nullopt;
    // 8228B310 marks the first local_count entrant descriptors as local;
    // 82289C20 also appends Loading previews in local-player order. Do not
    // test racer+108: 82280468 changes active control type for autopilot.
    const uint32_t count = std::min(racers->populated_count, racers->local_count);
    for (uint32_t slot = 0; slot < count; ++slot) {
        const uint32_t rider = memory.load<uint32_t>(uint64_t(racers->begin) + slot * 4);
        if (!rider || !memory.readable(rider, 3212) ||
            memory.load<uint32_t>(uint64_t(rider) + 100) != 17) continue;
        const uint32_t character = memory.load<uint32_t>(uint64_t(rider) + 3208);
        if (!character || !memory.readable(character, 20)) continue;
        LocalAvatar avatar{racers->manager, rider, character,
            memory.load<uint32_t>(uint64_t(character) + 8),
            memory.load<uint32_t>(uint64_t(character) + 12),
            memory.load<uint32_t>(uint64_t(character) + 16), slot, racers->local_count};
        if (!avatar.renderer || !memory.readable(avatar.renderer, 9)) continue;
        if (match(avatar)) return avatar;
    }
    return std::nullopt;
}

inline bool has_animation(const GuestMemory& memory, const LocalAvatar& avatar) {
    return avatar.controller && avatar.animation && memory.readable(avatar.controller, 4) &&
           memory.readable(avatar.animation, 4);
}
}

inline std::optional<LocalAvatar> local_avatar_for_renderer(const GuestMemory& memory, uint32_t renderer) {
    if (!renderer) return std::nullopt;
    return avatar_state_detail::find(memory, [renderer](const LocalAvatar& avatar) {
        return avatar.renderer == renderer;
    });
}

inline std::optional<LocalAvatar> local_avatar_for_animation(const GuestMemory& memory,
        uint32_t controller, uint32_t renderer, uint32_t animation) {
    if (!controller || !renderer || !animation) return std::nullopt;
    return avatar_state_detail::find(memory, [&](const LocalAvatar& avatar) {
        return avatar.controller == controller && avatar.renderer == renderer &&
               avatar.animation == animation && avatar_state_detail::has_animation(memory, avatar);
    });
}

inline std::optional<LocalAvatar> local_avatar_for_animation(const GuestMemory& memory, uint32_t animation) {
    if (!animation) return std::nullopt;
    return avatar_state_detail::find(memory, [&](const LocalAvatar& avatar) {
        return avatar.animation == animation && avatar_state_detail::has_animation(memory, avatar);
    });
}

// Compatibility for callers still restricted to a single local player. Model
// initialization is deliberately not required to establish selected identity.
inline uint32_t single_player_avatar_racer(const GuestMemory& memory) {
    const auto racers = avatar_state_detail::local_racers(memory);
    if (!racers || racers->local_count != 1) return 0;
    const uint32_t rider = memory.load<uint32_t>(racers->begin);
    if (!rider || !memory.readable(rider, 108)) return 0;
    // +100 is the character; 17/18 at +104 are Avatar body/gear variants.
    return memory.load<uint32_t>(uint64_t(rider) + 100) == 17 ? rider : 0;
}
}
