#pragma once
#include "native_input.h"
#include <cstdint>

namespace sfr {
class GuestMemory;

// Race controls from the pad. In a race the title reads the player's body
// state (a 4788-byte record at KinnectNuiBox+0x78 for P1, filled from the
// skeleton) and runs gesture detectors on it (vtables at 821A22xx, detect =
// vtable[1](detector, source, results)). Standing on a board in front of a
// sensor is replaced by the pad: the body record the detectors see is ours,
// filled below each frame, and the detectors that recognize whole-body
// motions answer from the pad instead (see nui_hooks.cpp).
//
//   left stick     lean (steer), tricks in the air, lever/handle gear
//   A hold/release crouch and charge / jump
//   X press/release kick dash (held X also paddles when swimming)
//   B hold         brake and grab
//   Y              switch stance (regular/goofy)
//   LB / RB        power skill (left / right)
//   RT             item and special-gear actions
//   right stick up arms up
struct RaceBody {
    float x_held_seconds = 0;   // +0
    float a_released = 0;       // +4 and +36 (1 on the frame A is released)
    float a_held = 0;           // +8
    float right_x = 0;          // +16
    float lean = 0;             // +32 (-left x)
    float left_y = 0;           // +40
    float x_pressed = 0;        // +48
    float x_released = 0;       // +52
    float y_pressed = 0;        // +72
    float left_x = 0;           // +88
    float right_trigger = 0;    // +92
    float right_y = 0;          // +96
    uint32_t left_trigger = 0;  // +256
    uint32_t lb_pressed = 0;    // +260
    uint32_t rb_pressed = 0;    // +264
    uint32_t b_held = 0;        // +268 and +272
    // +640 / +644: a pair the title compares for the lean (822C6200 and
    // others): 640 > 644 leans right by 640/644 - 1, 644 > 640 left by
    // 644/640 - 1, at most 3.5. Equal is upright; a zero +640 reads as a
    // full lean, which is why an untouched stick used to lean right.
    float lean_right = 1.0f;    // +640
    float lean_left = 1.0f;     // +644
    uint32_t hands = 0;         // +740 / +756 tracking state of both hands (2 = tracked)
};

constexpr uint32_t race_body_size = 4788;

// Stick axis in -1..1 with the XInput dead zones (left 7849, right 8689).
float race_axis(int16_t value, int16_t dead_zone);

class RaceInput {
public:
    // Advances one title frame of pad input (seconds since the last frame).
    void update(const GamepadState& pad, float seconds);
    const RaceBody& body() const { return body_; }
    // Writes the fields above into a zeroed body record.
    void write(GuestMemory& memory, uint32_t record) const;
private:
    RaceBody body_;
    uint16_t previous_ = 0;
};
}
