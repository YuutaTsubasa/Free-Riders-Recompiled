#include "nui_race.h"
#include "guest_memory.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>

namespace sfr {
float race_axis(int16_t value, int16_t dead_zone) {
    const float v = float(value);
    if (std::fabs(v) <= dead_zone) return 0.0f;
    const float span = 32767.0f - dead_zone;
    return std::clamp((v - std::copysign(float(dead_zone), v)) / span, -1.0f, 1.0f);
}

void RaceInput::update(const GamepadState& pad, float seconds) {
    namespace button = gamepad_button;
    const uint16_t held = pad.buttons, pressed = held & ~previous_, released = previous_ & ~held;
    previous_ = held;
    RaceBody& b = body_;
    b.left_x = race_axis(pad.thumb_lx, 7849);
    b.left_y = race_axis(pad.thumb_ly, 7849);
    b.right_x = race_axis(pad.thumb_rx, 8689);
    b.right_y = race_axis(pad.thumb_ry, 8689);
    // 0 - x rather than -x: negating a neutral stick gives negative zero,
    // whose sign bit reads as a lean to one side.
    b.lean = 0.0f - b.left_x;
    // A full stick is a lean of lean_scale (SFR_RACE_LEAN_SCALE, default 1).
    static const float lean_scale = [] {
        const char* text = std::getenv("SFR_RACE_LEAN_SCALE");
        const float value = text ? std::strtof(text, nullptr) : 1.0f;
        return std::clamp(value, 0.1f, 3.5f);
    }();
    b.lean_right = 1.0f + lean_scale * std::max(b.left_x, 0.0f);
    b.lean_left = 1.0f + lean_scale * std::max(-b.left_x, 0.0f);
    b.x_held_seconds = (held & button::x) ? b.x_held_seconds + seconds : 0.0f;
    b.a_held = (held & button::a) ? 1.0f : 0.0f;
    b.a_released = (released & button::a) ? 1.0f : 0.0f;
    b.x_pressed = (pressed & button::x) ? 1.0f : 0.0f;
    b.x_released = (released & button::x) ? 1.0f : 0.0f;
    b.y_pressed = (pressed & button::y) ? 1.0f : 0.0f;
    b.right_trigger = pad.right_trigger > 30 ? 1.0f : 0.0f;
    b.left_trigger = pad.left_trigger > 30 ? 1 : 0;
    b.lb_pressed = (pressed & button::left_shoulder) ? 1 : 0;
    b.rb_pressed = (pressed & button::right_shoulder) ? 1 : 0;
    b.b_held = (held & button::b) ? 1 : 0;
    // Hands count as tracked while they are used (crouching, braking) or
    // pulled back with the stick down.
    b.hands = (b.left_y <= -0.75f || b.a_held != 0 || b.b_held) ? 2 : 0;
}

void RaceInput::write(GuestMemory& memory, uint32_t record) const {
    const RaceBody& b = body_;
    auto f = [&](uint32_t offset, float value) {
        memory.store<uint32_t>(uint64_t(record) + offset, std::bit_cast<uint32_t>(value));
    };
    auto u = [&](uint32_t offset, uint32_t value) { memory.store<uint32_t>(uint64_t(record) + offset, value); };
    f(0, b.x_held_seconds);
    f(4, b.a_released);
    f(8, b.a_held);
    f(16, b.right_x);
    f(32, b.lean);
    f(36, b.a_released);
    f(40, b.left_y);
    f(48, b.x_pressed);
    f(52, b.x_released);
    f(72, b.y_pressed);
    f(88, b.left_x);
    f(92, b.right_trigger);
    f(96, b.right_y);
    u(256, b.left_trigger);
    u(260, b.lb_pressed);
    u(264, b.rb_pressed);
    u(268, b.b_held);
    u(272, b.b_held);
    f(640, b.lean_right);
    f(644, b.lean_left);
    u(740, b.hands);
    u(756, b.hands);
}
}
