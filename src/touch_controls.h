#pragma once
#include "native_input.h"
#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace sfr {
// On-screen controls for touch screens (Android, iOS): a floating stick on the
// left, A/B/X/Y, RT, a small right stick and the shoulder buttons around
// the game's HUD, drawn over the game by the presentation blit. Coordinates
// are relative to the game image (0..1 across its 16:9 area, y down); radii
// are fractions of its height.
//
//   stick   anywhere in the left 40% in a race, on its own circle elsewhere:
//           the base appears under the finger and the knob follows it. In a
//           race it is the left stick (leaning); in the menus the D-pad once
//           past half-way, one step per push (the left stick would move the
//           emulated Kinect hand there)
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
// stick X), and with "Tilt to lean" tipping its top edge away from the
// player leans forward, towards the player back (left stick Y; TiltLean
// below), unless the stick is held.
//
// Outside races the screen itself is the Kinect hand cursor (Issue #217): a
// finger that lands away from the controls is the pointer, and the hand
// cursor goes to it and follows it (the stick then starts only on its own
// circle). Selection stays the game's own: the cursor rests where the finger
// left it and the item under it fills and is chosen, as with a hand. On the
// title screen a touch away from the controls is START.
enum class TouchScene { menu, race, title };

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
    uint32_t readout = 0;  // the frame-rate readout's word (fps_readout.h), 0 hidden
};

// The blit's push constants are 32 words, the 128 bytes every Vulkan device
// offers: the sampled area (2 floats), then the overlay packed as pairs of
// half floats (src/shaders/blit.hlsl): the two knobs, then two words per
// circle, (x, y) and (radius, state). The last word is the frame-rate
// readout's (fps_readout.h).
constexpr size_t touch_constant_words = 30;
constexpr size_t touch_readout_word = touch_constant_words - 1;
static_assert(2 + TouchOverlay::circles * 2 <= touch_readout_word);
std::array<uint32_t, touch_constant_words> pack_touch_overlay(const TouchOverlay& overlay);
uint16_t float_to_half(float value);
float half_to_float(uint16_t half);

class TouchControls {
public:
    // Circle order is the blit's (blit.hlsl's colours): buttons a..lt.
    enum Button : size_t { stick = 0, a, b, x, y, trigger, start, lb, rb, lt, right_stick };
    // One frame of touches; tilt is the steering angle in -1..1 (0 when the
    // device is level or there is no sensor), lean the forward (+) / back (-)
    // lean in -1..1 (TiltLean); racing selects the stick's role.
    void update(std::span<const TouchPoint> touches, float tilt = 0.0f, bool racing = false) {
        update(touches, tilt, racing ? TouchScene::race : TouchScene::menu);
    }
    void update(std::span<const TouchPoint> touches, float tilt, TouchScene scene, float lean = 0.0f);
    GamepadState state() const { return state_; }
    // The pointer finger in the menus, in game-image coordinates clamped to
    // 0..1; none when no finger is the pointer.
    std::optional<std::array<float, 2>> pointer() const { return pointer_; }
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
    int64_t pointer_finger_ = -1;  // the menus' hand cursor, or START's on the title
    std::optional<std::array<float, 2>> pointer_;
    std::array<bool, TouchOverlay::circles> pressed_{};
};

// The phone's pitch in degrees from an accelerometer reading (the sensor's
// portrait axes, any unit) in landscape: 0 with the screen upright facing the
// player, +90 lying face up, past 90 with its top edge lower than the bottom.
// side is 1 in landscape (the device's right edge up), -1 flipped. Turning
// it like a wheel (the steering) leaves the pitch alone.
float tilt_pitch_degrees(float x, float y, float z, float side);

// "Tilt to lean" (Issue #247): forward / back lean from the phone's pitch,
// the left stick's Y on a pad. In a race that axis only feeds the trick and
// gesture detectors (record +40, nui_race.cpp); it neither speeds up nor
// brakes (X kicks, B brakes), so this mostly matters for spins. Tipping the
// top edge away from the player (the screen turning up to the sky) leans
// forward, as pushing a board's nose down does; towards the player leans
// back. People hold phones at any angle, so there is no fixed zero: the pitch
// when a race starts (when a reading is first valid in a race) is neutral,
// and it follows the phone slowly: within the dead zone in about a second (a
// grip shifting), beyond it in about half a minute (so a lean held for a
// straight hardly fades, but a new way of holding the phone becomes neutral).
// 5 degrees from neutral does nothing, 25 is a full lean, as with steering.
//
// Air tricks start from the stick turning during the ~0.25 s takeoff after
// A is released (the trick detector reads the stick's angle). Steering alone
// keeps the stick on its X axis, where turning the phone does not change the
// angle; a lean as well would. So while A is held (crouching to jump) and for
// half a second after it is released the lean is 0, and only steering is
// left. A takeoff without A (rolling off a ramp) is not seen; there the lean
// could still turn the stick a little and start a small trick.
class TiltLean {
public:
    static constexpr float dead_zone = 5.0f, full = 25.0f, after_jump = 0.5f;
    // One reading: pitch in degrees (tilt_pitch_degrees), valid when there is
    // a landscape reading, racing in a race, jumping while A is held, seconds
    // since the last call. Returns the lean in -1..1.
    float update(float pitch, bool valid, bool racing, bool jumping, float seconds);
    bool calibrated() const { return calibrated_; }
    float neutral() const { return neutral_; }

private:
    bool calibrated_ = false;
    float neutral_ = 0.0f;
    float quiet_ = 0.0f;  // seconds the lean stays 0 after A
};

// Where the Kinect hand cursor should go (Issue #217): the pointer finger,
// or on a desktop the mouse moved over the game. Positions are game-image
// coordinates (0..1); each new position gets a new serial, so a resting
// pointer leaves the hand to the stick.
struct PointerTarget {
    float x = 0.0f, y = 0.0f;
    uint32_t serial = 0;  // 0 until the first one
    bool held = false;    // a finger is on it (the mouse never holds)
};
// landed: a finger that has just touched down (a new target even where the
// last one was).
void publish_pointer(float x, float y, bool landed = false, bool held = false);
// The finger left the screen.
void release_pointer();
PointerTarget pointer_target();
// The window-to-game-image mapping of the last presented frame (the blit's
// viewport in a window of out_width x out_height), for the mouse.
void set_pointer_view(float x, float y, float width, float height, float out_width, float out_height);
// A window position (0..1 across the window's width and height) as
// game-image coordinates; none outside the image.
std::optional<std::array<float, 2>> pointer_from_window(float x, float y);
// Whether the title screen waits for START (the Kinect hooks know); with
// touch_racing, the scene the touches are read for.
void set_touch_title(bool waiting);
TouchScene touch_scene();

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
// SFR_TOUCH_CONTROLS: 1 on Android and iOS by default, 0 elsewhere.
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

// Whether the buttons carry their names and LB/RB their arrows
// (touch_labels.h; SFR_TOUCH_LABELS=0 hides them, Issue #236). Hidden, the
// readout's word (fps_readout.h) has this bit set.
constexpr uint32_t touch_names_hidden_bit = 1u << 17;
bool touch_names_shown();
}
