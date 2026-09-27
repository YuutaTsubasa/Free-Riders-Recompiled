#include "pad_assignment.h"

#include <algorithm>

namespace sfr {

void PadAssignment::prefer(uint32_t player, uint64_t pad) {
    if (player < players) wanted_[player] = pad;
}

void PadAssignment::enable(uint32_t player, bool enabled) {
    if (player < players) enabled_[player] = enabled;
}

void PadAssignment::update(std::span<const uint64_t> connected) {
    const auto here = [&](uint64_t pad) { return std::find(connected.begin(), connected.end(), pad) != connected.end(); };
    const auto asked_for = [&](uint64_t pad) {
        for (uint32_t player = 0; player < players; ++player)
            if (enabled_[player] && wanted_[player] == pad) return true;
        return false;
    };
    // A controller that has gone gives its number back; the rest keep theirs.
    for (uint32_t player = 0; player < players; ++player)
        if (!enabled_[player] || !here(pads_[player])) pads_[player] = no_pad;
    // A player who asked for one particular controller has that one and no
    // other: while it is connected it is theirs, taken off whoever had it,
    // and while it is not they wait rather than being handed a stranger's.
    for (uint32_t player = 0; player < players; ++player) {
        if (!enabled_[player] || wanted_[player] == no_pad) continue;
        if (const auto holder = player_of(wanted_[player]); holder && *holder != player) pads_[*holder] = no_pad;
        pads_[player] = here(wanted_[player]) ? wanted_[player] : no_pad;
    }
    // What is left goes, in the host's own order, to the players who did not
    // ask for anything in particular.
    for (const uint64_t pad : connected) {
        if (player_of(pad) || asked_for(pad)) continue;
        for (uint32_t player = 0; player < players; ++player)
            if (enabled_[player] && pads_[player] == no_pad && wanted_[player] == no_pad) { pads_[player] = pad; break; }
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
