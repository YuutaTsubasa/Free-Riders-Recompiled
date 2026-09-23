#include "pad_assignment.h"

#include <algorithm>

namespace sfr {

void PadAssignment::update(std::span<const uint64_t> connected) {
    const auto here = [&](uint64_t pad) { return std::find(connected.begin(), connected.end(), pad) != connected.end(); };
    // A controller that has gone gives its number back; the rest keep theirs.
    for (uint64_t& pad : pads_)
        if (pad != no_pad && !here(pad)) pad = no_pad;
    for (const uint64_t pad : connected) {
        if (player_of(pad)) continue;
        const auto free = std::find(pads_.begin(), pads_.end(), no_pad);
        if (free == pads_.end()) break;  // four players is all the title has
        *free = pad;
    }
}

uint64_t PadAssignment::pad_of(uint32_t player) const {
    return player < players ? pads_[player] : no_pad;
}

std::optional<uint32_t> PadAssignment::player_of(uint64_t pad) const {
    if (pad == no_pad) return std::nullopt;
    const auto found = std::find(pads_.begin(), pads_.end(), pad);
    if (found == pads_.end()) return std::nullopt;
    return uint32_t(found - pads_.begin());
}

}
