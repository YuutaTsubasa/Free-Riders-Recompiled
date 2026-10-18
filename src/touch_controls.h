#pragma once
#include "native_input.h"
#include <array>
#include <cstdint>
#include <span>

namespace sfr {
// On-screen controls for touch screens (Android): a floating stick on the
// left, A/B/X/Y, RT, a small right stick and the shoulder buttons around
// the game's HUD, drawn over the game by the presentation blit. Coordinates
// are relative to the game image (0..1 across its 16:9 area, y down); radii
// are fractions of its height.
//
//   stick   anywhere in the left 40%: the base appears under the finger and
//           the knob follows it. In a race it is the left stick (leaning); in
//           the menus the D-pad once past half-way, one step per push (the
//           left stick would move the emulated Kinect hand there)
//   A       bottom of the diamond  (green)     hold to crouch, release to jump
//   B       right of the diamond   (red)       back; brake / grab
//   X       left of the diamond    (blue)      kick dash
//   Y       top of the diamond     (yellow)    stance
//   RT      left of X              (grey)      use / shake an item
//   START   top centre             (white)     start; pause in a race
//   LB, RB  left / right edge, between the HUD's corners (orange, with an
//           arrow): the power skill, a punch to that side (Issue #177)
//   RS      up and left of the diamond (cyan): the right stick, a fixed base
//           whose knob follows the finger that started on it. In a race it
//           stretches the hands: sideways reaches for Rings beside the board
//           (further the further it is pushed), up raises both arms; held with
//           RT it wipes steam. In the menus it moves the hand cursor
//   LT      bottom, right of the stick (grey)  hold hands (Tag Race)
//
// In a race, tilting the device like a steering wheel also steers (left
// stick X), unless the stick is held.
struct TouchPoint {
    int64_t id;
    float x, y;  // game-image coordinates; may lie outside 0..1 (black bars)
};

// What the blit draws: per circle (x, y, radius, state), state 0 hidden,
// 1 shown, 2 pressed. The circles are TouchControls::Button's; knob and
// right_knob are the knobs of the stick and the right stick.
struct TouchOverlay {
    static constexpr size_t circles = 11;
    std::array<std::array<float, 4>, circles> circle{};
    std::array<float, 2> knob{}, right_knob{};
};

// The blit's push constants are 32 words, the 128 bytes every Vulkan device
// offers: the sampled area (2 floats), then the overlay packed as pairs of
// half floats (src/shaders/blit.hlsl): the two knobs, then two words per
// circle, (x, y) and (radius, state).
constexpr size_t touch_constant_words = 30;
static_assert(2 + TouchOverlay::circles * 2 <= touch_constant_words);
std::array<uint32_t, touch_constant_words> pack_touch_overlay(const TouchOverlay& overlay);
uint16_t float_to_half(float value);
float half_to_float(uint16_t half);

class TouchControls {
public:
    // Circle order is the blit's (blit.hlsl's colours): buttons a..lt.
    enum Button : size_t { stick = 0, a, b, x, y, trigger, start, lb, rb, lt, right_stick };
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
    int64_t right_finger_ = -1;
    std::array<float, 2> right_knob_{};
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
// The Pause Gesture's gauge (pause_gesture.h), as the console showed
// it: 0 hides it, otherwise how far the hold has got (0..1). The blit draws
// it as a ring in the lower left, in the START button's slot.
void publish_pause_gauge(float progress);
float pause_gauge();
// SFR_TOUCH_CONTROLS: 1 on Android by default, 0 elsewhere.
bool touch_controls_enabled();
// A controller answered for player 1 (native_input). The on-screen buttons
// are drawn and read when none is connected, and also with one whenever the
// screen was touched after the controller's last input: a touch brings them
// back, the controller's next button or stick hides them. Android lists some
// keyboards and built-in keys as game controllers (Issue #177, a POCO X3 GT:
// the buttons vanished as the game started and never came back).
void note_controller(bool present);
void note_controller_input();
void note_touch();
bool touch_controls_active();
}
