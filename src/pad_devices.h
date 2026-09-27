#pragma once
#include "input_bindings.h"
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sfr {

// A controller that is connected now: the id it is known by for as long as
// it stays plugged in (an XInput slot, an SDL instance id, or sony_pad_id),
// and a name to show a player.
struct PadDevice {
    uint64_t id = 0;
    std::string name;
    std::string identity;     // persistent setting value; empty uses name (Windows)
    std::string legacy_name;  // SDL's original model name, accepted only if unique
};

struct SdlPadIdentity {
    uint64_t id;
    std::string name, guid, serial;
};
// Serial numbers keep an identity across enumeration changes. Without one,
// use the occurrence within the GUID group: reconnecting identical pads in
// another order can swap their identities, so users must reselect them.
std::vector<PadDevice> identify_sdl_pads(std::span<const SdlPadIdentity> devices);

// PlayStation controllers are read over HID rather than XInput
// (sony_gamepad.h), so they need an id of their own, well past the four
// XInput slots.
constexpr uint64_t sony_pad_id = 100;

// The controllers connected now, in the host's own order. Both the launcher
// (to offer them) and the game (to find the one a player asked for) ask for
// this list, so a name means the same thing on both sides.
std::vector<PadDevice> connected_pads();

// The controller with that name. An empty name is no request at all
// (PadAssignment::no_pad), which is what a player who has not chosen gets;
// a name that nothing connected answers to is PadAssignment::absent_pad, so
// that player waits for it rather than being given the next one along.
uint64_t pad_with_name(const std::string& name);
uint64_t pad_with_name(const std::string& name, std::span<const PadDevice> devices);

struct PadSample { uint64_t id; GamepadState state; };
// All controllers, independent of the controller used for menu navigation.
std::vector<PadSample> poll_pad_buttons();
class PadButtonCapture {
public:
    void reset(std::span<const PadSample> samples);
    std::optional<InputAction> update(std::span<const PadSample> samples);
private:
    std::vector<PadSample> previous_;
};

}
