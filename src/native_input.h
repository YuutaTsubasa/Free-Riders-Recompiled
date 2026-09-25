#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace sfr {
class GuestMemory;

// XINPUT_GAMEPAD; Xbox 360 XAM and Windows XInput share the button bits.
struct GamepadState {
    uint16_t buttons = 0;
    uint8_t left_trigger = 0, right_trigger = 0;
    int16_t thumb_lx = 0, thumb_ly = 0, thumb_rx = 0, thumb_ry = 0;
    bool operator==(const GamepadState&) const = default;
};

namespace gamepad_button {
constexpr uint16_t dpad_up = 0x0001, dpad_down = 0x0002, dpad_left = 0x0004, dpad_right = 0x0008;
constexpr uint16_t start = 0x0010, back = 0x0020, left_thumb = 0x0040, right_thumb = 0x0080;
constexpr uint16_t left_shoulder = 0x0100, right_shoulder = 0x0200;
constexpr uint16_t a = 0x1000, b = 0x2000, x = 0x4000, y = 0x8000;
}

constexpr uint32_t xinput_success = 0, xinput_not_connected = 0x48F;
constexpr uint32_t xinput_state_size = 16;

// Keyboard fallback for user 0. down(virtual_key) reports a held Windows key.
// Arrows: D-pad and left stick; Enter: START; Tab: BACK; Z/Space: A;
// X/Backspace/Escape: B; C: X; V: Y; Q/E: shoulders; F/R: right/left
// trigger; I/J/K/L: right stick.
GamepadState keyboard_gamepad(const std::function<bool(int)>& down);
// Scripted presses for unattended runs (SFR_INPUT_SCRIPT): comma-separated
// "button@seconds[+duration]" entries, e.g. "start@95,a@110+0.5"; duration
// defaults to 0.25 s. Buttons: start back a b x y up down left right lb rb;
// stick directions: lup ldown lleft lright rup rdown rleft rright.
// Returns the buttons held at the given time since start; malformed scripts throw.
GamepadState scripted_gamepad(const std::string& script, double seconds);
// Buttons are OR-ed; each axis keeps the value farther from rest.
GamepadState merge_gamepads(const GamepadState& first, const GamepadState& second);
// Big-endian XINPUT_STATE: packet number, then XINPUT_GAMEPAD.
void write_xinput_state(GuestMemory& memory, uint32_t address, uint32_t packet, const GamepadState& state);

class NativeInput {
public:
    // pad(user) reads a host controller; keyboard() reads the fallback keys
    // (returns rest unless the game window has focus).
    NativeInput(std::function<std::optional<GamepadState>(uint32_t)> pad,
                std::function<GamepadState()> keyboard);
    // Returns xinput_success or xinput_not_connected; always writes all 16 bytes
    // when connected. User 0 is always connected because the keyboard backs it.
    // Safe to call from several guest threads at once: the title polls input
    // without the global execution permit (diagnostic_main.cpp's permit_free),
    // and the packet numbers it keeps are this object's own state.
    uint32_t get_state(GuestMemory& memory, uint32_t user, uint32_t output);
    // The merged state of user 0 (pad, keyboard, script) or a host pad; null
    // when that user has no controller.
    std::optional<GamepadState> current(uint32_t user) const;
    // The controller alone, with nothing merged into it: null when that
    // player is not holding one. User 0's current() is keyboard-backed and so
    // never null, which says nothing about whether anybody is there.
    std::optional<GamepadState> controller(uint32_t user) const;
    // XamInputSetState: forward rumble motor speeds to a host pad when present.
    // User 0 is always connected (keyboard-backed), so it always succeeds.
    uint32_t set_vibration(uint32_t user, uint16_t left_motor, uint16_t right_motor);
    // script_clock (optional) gives the SFR_INPUT_SCRIPT time in seconds,
    // negative before the script starts; by default time since creation.
    static NativeInput windows(std::function<void*()> focus_window, std::function<double()> script_clock = {});
    // SDL game controllers, and the keyboard while focus_window (an
    // SDL_Window) has keyboard focus. Events come from the presentation's pump.
    static NativeInput sdl(std::function<void*()> focus_window, std::function<double()> script_clock = {});
    // windows() on Windows, sdl() elsewhere.
    static NativeInput host(std::function<void*()> focus_window, std::function<double()> script_clock = {});
private:
    void attach_script(std::function<double()> script_clock);
    std::function<std::optional<GamepadState>(uint32_t)> pad_;
    std::function<GamepadState()> keyboard_;
    std::function<GamepadState()> script_ = [] { return GamepadState{}; };
    std::function<bool(uint32_t, uint16_t, uint16_t)> vibrate_ = [](uint32_t, uint16_t, uint16_t) { return false; };
    // Held only over last_ and packets_. Indirect because a NativeInput is
    // built by a factory and copied out of it, and a mutex is neither
    // copyable nor movable.
    std::shared_ptr<std::mutex> mutex_ = std::make_shared<std::mutex>();
    std::array<GamepadState, 4> last_{};
    std::array<uint32_t, 4> packets_{};
};
}
