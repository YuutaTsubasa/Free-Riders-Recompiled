#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace sfr {

// Which player each host controller is.
//
// A controller keeps its player number for as long as it stays connected, so
// plugging a second one in never moves the first. Without that, a controller
// is whatever place the host lists it in today: Windows gives a newly
// connected XInput pad the lowest free slot, and a PlayStation pad -- which
// XInput cannot see at all -- was only ever read as player one, so the new
// pad took player one and the pad already being played with went silent.
//
// The controllers are named by an id that lasts as long as the device is
// connected: an XInput slot, an SDL instance id. update() is handed the ids
// that are connected now, in the host's own order, and gives the free numbers
// out in that order.
class PadAssignment {
public:
    static constexpr uint32_t players = 4;
    static constexpr uint64_t no_pad = ~uint64_t(0);
    // A controller a player asked for that is not connected at the moment:
    // it can never be in the list, so that player waits for it instead of
    // being handed the next one along.
    static constexpr uint64_t absent_pad = ~uint64_t(0) - 1;

    // A player may ask for one particular controller (the launcher's
    // Controls page names it). While that controller is connected it is
    // theirs, whatever order the host lists it in, and nobody else may be
    // given it; when it is not, that player waits rather than being handed
    // somebody else's. no_pad clears the request.
    void prefer(uint32_t player, uint64_t pad);
    // Keyboard-only and off slots neither receive nor reserve controllers.
    void enable(uint32_t player, bool enabled);
    void update(std::span<const uint64_t> connected);
    // The controller playing as this player, or no_pad.
    uint64_t pad_of(uint32_t player) const;
    // Which player a controller is, if it is one.
    std::optional<uint32_t> player_of(uint64_t pad) const;

private:
    std::array<uint64_t, players> pads_{no_pad, no_pad, no_pad, no_pad};
    std::array<uint64_t, players> wanted_{no_pad, no_pad, no_pad, no_pad};
    std::array<bool, players> enabled_{true, true, true, true};
};

}
