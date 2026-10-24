#pragma once

// Which edge of a wide frame a race HUD sprite belongs to, by the title
// function that draws it (guest_graphics_hooks.cpp, HUD anchoring), never by
// where it is: text, the results, the pause ring and anything not listed stay
// together in the middle.
//
// A HUD sprite goes through the title's generic UI layer (the sprite drawer
// 824C06E0 returning to 824C08A4, its callers 824C0C88/824C0D78, 824AC17C/
// 824AC1C8, the node drawer 824AD1A0, which recurses, and the tree's root
// 824AE4F0, and the textured-quad path 824C6A04/8223CED8 the map itself is
// drawn through: Issue #145, where the map stayed in the middle while its
// markers went to the edge); the first return address above that layer is
// the HUD element's own function. Found with the guest return-address chain
// of every 2D draw (SFR_ASPECT_TRACE) in scripted one- and two-player races
// at 22:9 (2026-10-16) and a World Grand Prix Ring Challenge (2026-10-25).

#include <algorithm>
#include <cstdint>
#include <iterator>

namespace sfr::race_hud {

// -1 left edge, +1 right edge, by_half, or 0: stays in the middle.
constexpr int by_half = 2;

struct Owner { uint32_t site; int8_t one_player, two_players; };

inline constexpr Owner owners[] = {
    {0x82401C9C, -1, -1}, {0x82401CE0, -1, -1},  // rings, LV
    {0x824023BC, -1, -1}, {0x82402404, -1, -1},  // skills
    {0x82402088, -1, -1},                        // the left column's frame and effects
    {0x82402300, -1, -1},                        // rank
    {0x82400624, -1, -1},                        // rank effects
    {0x8240069C, -1, -1},                        // LAP, RECORD
    {0x823EFF80, +1, +1}, {0x82403158, +1, +1},  // map and its markers
    {0x823F97E4, +1, by_half},                   // speed gauge (one per player)
    {0x823F9AF4, +1, by_half},                   // SPEED
    {0x823F9D8C, by_half, by_half},              // effects at both edges
    {0x823FC63C, 0, by_half},                    // gauge scale
    {0x82240F74, 0, by_half},                    // a two-player race's per-player time
    {0x823F09C8, 0, by_half},
    // A World Grand Prix mission's counters (sub_823FE940, Issue #274): it
    // switches on the mission type and draws that type's panel pieces (one
    // loop of nodes per type) and its digits (three counters, then the rest).
    // In a Ring Challenge they are the ring count and target at the top left
    // and the record's rings at the bottom left, inside the left column's
    // frame (82402088), which goes to the left edge: without them the frame
    // moved and its numbers stayed. Each piece goes with its half of the
    // layout, as the other mission types' pieces were not traced.
    {0x823FE9E8, by_half, by_half}, {0x823FEA48, by_half, by_half},
    {0x823FEAA8, by_half, by_half}, {0x823FEB48, by_half, by_half},
    {0x823FEBB8, by_half, by_half},                                  // panel pieces, by mission type
    {0x823FEAC8, by_half, by_half}, {0x823FEAE0, by_half, by_half},
    {0x823FEBD8, by_half, by_half}, {0x823FEBF0, by_half, by_half},  // the counters' digits
    {0x823FEC28, by_half, by_half},                                  // the remaining counters
};

inline constexpr uint32_t generic_layer[] = {0x824C0C88, 0x824C0D78, 0x824AC17C, 0x824AC1C8,
                                             0x824AD1A0, 0x824AE4F0, 0x824C6A04, 0x8223CED8};

inline bool generic(uint32_t site) {
    return std::find(std::begin(generic_layer), std::end(generic_layer), site) != std::end(generic_layer);
}

// The side the element whose function returns to `site` takes.
inline int owner_side(uint32_t site, bool two_players) {
    for (const auto& owner : owners)
        if (owner.site == site) return two_players ? owner.two_players : owner.one_player;
    return 0;
}

// The side of a draw from its return addresses, innermost first: the first
// one outside the generic UI layer decides.
template <typename Sites>
int chain_side(const Sites& sites, bool two_players) {
    for (const uint32_t site : sites)
        if (!generic(site)) return owner_side(site, two_players);
    return 0;
}

// by_half: the half of the title's layout the draw's horizontal extent is in.
inline int half_side(float lo, float hi, float layout_width) {
    return (lo + hi) * 0.5f < layout_width * 0.5f ? -1 : +1;
}

}  // namespace sfr::race_hud
