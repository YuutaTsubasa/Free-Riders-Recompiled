#pragma once
#include "native_input.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace sfr {

// What a player can bind. These are the Xbox pad's own controls, because
// that is what the title reads: whatever anybody presses arrives as one of
// these. The four stick directions are here too -- a key can only be a
// direction, and the Kinect menu cursor is the right stick.
enum class InputAction : uint8_t {
    a, b, x, y,
    left_shoulder, right_shoulder, left_trigger, right_trigger,
    start, back, left_thumb, right_thumb,
    dpad_up, dpad_down, dpad_left, dpad_right,
    left_stick_up, left_stick_down, left_stick_left, left_stick_right,
    right_stick_up, right_stick_down, right_stick_left, right_stick_right,
    count
};
constexpr size_t input_action_count = size_t(InputAction::count);

// The name a setting or an environment variable uses ("a", "left_shoulder",
// "right_stick_up"), and the way back.
const char* input_action_name(InputAction action);
std::optional<InputAction> input_action_from_name(std::string_view name);

// A key, as Windows numbers them (the SDL build translates). Zero is "not
// bound". Named where a name reads better than a number -- "Z", "Enter",
// "Left", "Numpad 8" -- and a number otherwise, so a hand-edited settings
// file can say either.
std::string key_name(int key);
int key_from_name(std::string_view name);

// One player's bindings. `key` is what to press on the keyboard for each
// action; `source` is which of the pad's own controls stands for it, as an
// InputAction (the pad's A button is InputAction::a). Either may be unset:
// key 0 and source count mean "nothing".
struct InputBindings {
    std::array<int, input_action_count> key{};
    std::array<InputAction, input_action_count> source{};

    InputBindings();
    int& operator[](InputAction action) { return key[size_t(action)]; }
    int operator[](InputAction action) const { return key[size_t(action)]; }
    InputAction& from(InputAction action) { return source[size_t(action)]; }
    InputAction from(InputAction action) const { return source[size_t(action)]; }
};

// What each player starts with. The first player keeps the keys this always
// had (arrows, Z, X, Enter); the second gets a set that does not collide
// with them, so two people can share one keyboard without rebinding
// anything first.
InputBindings default_bindings(uint32_t player);

// "a=Z,b=X,start=Enter": the keys, or the pad sources, as settings.ini and
// the game's environment carry them. Reading keeps whatever the text does
// not mention, so an older file or a partial line still works.
std::string format_keys(const InputBindings& bindings);
std::string format_pad(const InputBindings& bindings);
void read_keys(std::string_view text, InputBindings& bindings);
void read_pad(std::string_view text, InputBindings& bindings);

// The pad state a keyboard produces through these bindings. `down` answers
// whether a key is held.
GamepadState keyboard_state(const InputBindings& bindings, const std::function<bool(int)>& down);
// The same for a pad whose buttons are being used for other actions: the
// state as the title should see it, from the state the host reported.
GamepadState remap_pad(const InputBindings& bindings, const GamepadState& raw);

// Whether the bindings leave the pad exactly as it is, which is the common
// case and lets the remap be skipped.
bool pad_is_unchanged(const InputBindings& bindings);

// Which devices a player uses. The first player has both by default -- the
// keyboard has always backed the pad -- and the second has none until
// somebody plugs a pad in, which is what makes them join.
enum class PlayerDevice : uint8_t { both, gamepad, keyboard, off };
const char* player_device_name(PlayerDevice device);
PlayerDevice player_device_from_name(std::string_view name, PlayerDevice fallback);

}
