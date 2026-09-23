#pragma once
#include "native_input.h"
#include <array>
#include <cstdint>
#include <span>

namespace sfr {
// On-screen controls for touch screens (Android): a floating stick on the
// left and A/B/X/Y, RT and START on the right, drawn over the game by the
// presentation blit. Coordinates are relative to the game image (0..1 across
// its 16:9 area, y down); radii are fractions of its height.
//
//   stick  anywhere in the left 40%: the base appears under the finger and the
//          knob follows it. In a race it is the left stick (leaning); in the
//          menus the D-pad once past half-way, one step per push (the left
//          stick would move the emulated Kinect hand there)
//   A      bottom of the diamond  (green)     hold to crouch, release to jump
//   B      right of the diamond   (red)       back; brake / grab
//   X      left of the diamond    (blue)      kick dash
//   Y      top of the diamond     (yellow)    stance
//   RT     left of X              (grey)      use / shake an item
//   START  top centre             (white)     start; pause in a race
//
// In a race, tilting the device like a steering wheel also steers (left
// stick X), unless the stick is held.
struct TouchPoint {
    int64_t id;
    float x, y;  // game-image coordinates; may lie outside 0..1 (black bars)
};

// What the blit draws: per circle (x, y, radius, state), state 0 hidden,
// 1 shown, 2 pressed. Circle 0 is the stick base; knob is the stick's knob.
struct TouchOverlay {
    static constexpr size_t circles = 7;
    std::array<std::array<float, 4>, circles> circle{};
    std::array<float, 2> knob{};
};

class TouchControls {
public:
    enum Button : size_t { stick = 0, a, b, x, y, trigger, start };
    // One frame of touches; tilt is the steering angle in -1..1 (0 when the
    // device is level or there is no sensor); racing selects the stick's role.
    void update(std::span<const TouchPoint> touches, float tilt = 0.0f, bool racing = false);
    GamepadState state() const { return state_; }
    TouchOverlay overlay() const;
    // Resting layout (circle centre and radius) of each control.
    static std::array<float, 3> layout(Button button);

private:
    GamepadState state_{};
    bool stick_active_ = false;
    int64_t stick_finger_ = -1;
    std::array<float, 2> stick_base_{}, stick_knob_{};
    std::array<bool, TouchOverlay::circles> pressed_{};
};

// The latest touch state, published by the thread that reads touches (the
// presentation's event pump) for the input and the blit.
void publish_touch_controls(const GamepadState& state, const TouchOverlay& overlay);
GamepadState touch_gamepad();
TouchOverlay touch_overlay();
// Whether a race is running (the Kinect hooks know), for the stick's role.
void set_touch_racing(bool racing);
bool touch_racing();
// SFR_TOUCH_CONTROLS: 1 on Android by default, 0 elsewhere.
bool touch_controls_enabled();
// A controller answered for player 1 (native_input): the on-screen buttons
// are not drawn or read while one is connected.
void note_controller(bool present);
bool touch_controls_active();
}
