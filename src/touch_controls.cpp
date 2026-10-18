#include "touch_controls.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <mutex>

namespace sfr {
namespace {
constexpr float aspect = 16.0f / 9.0f;       // game image width / height
constexpr float stick_region = 0.40f;        // left part of the image for the stick
constexpr float touch_slack = 1.25f;         // a button reacts a little outside its circle

// Distance in image heights.
float distance(float x0, float y0, float x1, float y1) {
    return std::hypot((x1 - x0) * aspect, y1 - y0);
}

int16_t axis(float value) { return int16_t(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f)); }

std::mutex published_mutex;
std::atomic<bool> racing_now{false};
GamepadState published_state;
// The controls at rest until touches are first read (never, on Windows,
// which has no touch reader: SFR_TOUCH_CONTROLS=1 shows them there).
TouchOverlay published_overlay = TouchControls{}.overlay();
}

std::array<float, 3> TouchControls::layout(Button button) {
    constexpr float cx = 0.845f, cy = 0.70f, step = 0.12f, r = 0.075f;
    switch (button) {
    case stick: return {0.16f, 0.70f, 0.13f};
    case a: return {cx, cy + step, r};
    case b: return {cx + step / aspect, cy, r};
    case x: return {cx - step / aspect, cy, r};
    case y: return {cx, cy - step, r};
    case trigger: return {0.66f, 0.84f, 0.065f};
    case start: return {0.5f, 0.08f, 0.05f};
    // The race HUD keeps the screen's edges free between its corners (Rings
    // and items above, the place and lap below on the left; the map above,
    // the air gauge below on the right), so the punches sit there, each on
    // its own side.
    case lb: return {0.065f, 0.44f, 0.07f};
    case rb: return {0.935f, 0.44f, 0.07f};
    case lt: return {0.34f, 0.86f, 0.055f};
    case right_stick: return {0.70f, 0.46f, 0.09f};
    }
    return {};
}

namespace {
// Where a finger on a stick puts its knob: (dx, dy) in -1..1 from the base.
std::array<float, 2> stick_offset(const TouchPoint& finger, std::array<float, 2> base, float radius) {
    float dx = (finger.x - base[0]) * aspect / radius;
    float dy = (finger.y - base[1]) / radius;
    const float length = std::hypot(dx, dy);
    if (length > 1.0f) { dx /= length; dy /= length; }
    return {dx, dy};
}
}

void TouchControls::update(std::span<const TouchPoint> touches, float tilt, bool racing) {
    namespace button = gamepad_button;
    state_ = {};
    pressed_.fill(false);
    // Each stick follows the finger that started it until that finger lifts.
    const auto held = [&](int64_t finger) {
        return std::find_if(touches.begin(), touches.end(), [&](const TouchPoint& t) { return t.id == finger; });
    };
    if (held(stick_finger_) == touches.end()) {
        stick_active_ = false;
        stick_finger_ = -1;
    }
    if (held(right_finger_) == touches.end()) right_finger_ = -1;
    const float stick_radius = layout(stick)[2];
    const auto [right_x, right_y, right_radius] = layout(right_stick);
    for (const TouchPoint& touch : touches) {
        if (touch.id == stick_finger_ || touch.id == right_finger_) continue;
        bool on_button = false;
        for (size_t index = a; index <= lt; ++index) {
            const auto [bx, by, br] = layout(Button(index));
            if (distance(bx, by, touch.x, touch.y) <= br * touch_slack) {
                pressed_[index] = true;
                on_button = true;
            }
        }
        if (on_button) continue;
        if (right_finger_ < 0 && distance(right_x, right_y, touch.x, touch.y) <= right_radius * touch_slack) {
            right_finger_ = touch.id;
        } else if (!stick_active_ && touch.x < stick_region && touch.x >= 0.0f) {
            stick_active_ = true;
            stick_finger_ = touch.id;
            stick_base_ = {touch.x, std::clamp(touch.y, stick_radius, 1.0f - stick_radius)};
        }
    }
    if (stick_active_) {
        const auto [dx, dy] = stick_offset(*held(stick_finger_), stick_base_, stick_radius);
        stick_knob_ = {stick_base_[0] + dx * stick_radius / aspect, stick_base_[1] + dy * stick_radius};
        if (racing) {
            state_.thumb_lx = axis(dx);
            state_.thumb_ly = axis(-dy);
        } else if (std::max(std::abs(dx), std::abs(dy)) > 0.5f) {
            // Menus move with the D-pad: its direction once past half-way.
            if (std::abs(dx) > std::abs(dy)) state_.buttons |= dx < 0 ? button::dpad_left : button::dpad_right;
            else state_.buttons |= dy < 0 ? button::dpad_up : button::dpad_down;
        }
        pressed_[stick] = true;
    } else if (racing && tilt != 0.0f) {
        state_.thumb_lx = axis(tilt);
    }
    right_knob_ = {right_x, right_y};
    if (right_finger_ >= 0) {
        // The right stick is analog everywhere: the race's reach and arms up,
        // the menus' hand cursor. The race's own dead zone ignores a resting
        // thumb's wobble.
        const auto [dx, dy] = stick_offset(*held(right_finger_), {right_x, right_y}, right_radius);
        right_knob_ = {right_x + dx * right_radius / aspect, right_y + dy * right_radius};
        state_.thumb_rx = axis(dx);
        state_.thumb_ry = axis(-dy);
        pressed_[right_stick] = true;
    }
    if (pressed_[a]) state_.buttons |= button::a;
    if (pressed_[b]) state_.buttons |= button::b;
    if (pressed_[x]) state_.buttons |= button::x;
    if (pressed_[y]) state_.buttons |= button::y;
    if (pressed_[start]) state_.buttons |= button::start;
    if (pressed_[lb]) state_.buttons |= button::left_shoulder;
    if (pressed_[rb]) state_.buttons |= button::right_shoulder;
    if (pressed_[trigger]) state_.right_trigger = 255;
    if (pressed_[lt]) state_.left_trigger = 255;
}

TouchOverlay TouchControls::overlay() const {
    TouchOverlay overlay;
    for (size_t index = 0; index < TouchOverlay::circles; ++index) {
        auto [cx, cy, radius] = layout(Button(index));
        if (index == stick && stick_active_) { cx = stick_base_[0]; cy = stick_base_[1]; }
        overlay.circle[index] = {cx, cy, radius, pressed_[index] ? 2.0f : 1.0f};
    }
    overlay.knob = stick_active_ ? stick_knob_ : std::array<float, 2>{layout(stick)[0], layout(stick)[1]};
    overlay.right_knob = right_knob_;
    return overlay;
}

uint16_t float_to_half(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    const uint16_t sign = uint16_t((bits >> 16) & 0x8000);
    const uint32_t biased = (bits >> 23) & 0xFF;
    uint32_t mantissa = bits & 0x7FFFFF;
    if (biased == 0xFF) return uint16_t(sign | 0x7C00 | (mantissa ? 0x200 : 0));
    const int exponent = int(biased) - 127 + 15;
    if (exponent >= 31) return uint16_t(sign | 0x7C00);
    if (exponent <= 0) {  // a subnormal half, or zero
        if (exponent < -10) return sign;
        mantissa |= 0x800000;
        const int shift = 14 - exponent;
        uint32_t half = mantissa >> shift;
        if ((mantissa >> (shift - 1)) & 1) ++half;
        return uint16_t(sign | half);
    }
    // Rounded to nearest; a carry out of the mantissa steps the exponent.
    uint32_t half = (uint32_t(exponent) << 10) | (mantissa >> 13);
    if (mantissa & 0x1000) ++half;
    return uint16_t(sign | (half > 0x7C00 ? 0x7C00 : half));
}

float half_to_float(uint16_t half) {
    const uint32_t exponent = (half >> 10) & 0x1F, mantissa = half & 0x3FF;
    float magnitude;
    if (exponent == 0) magnitude = std::ldexp(float(mantissa), -24);
    else if (exponent == 31) magnitude = mantissa ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    else magnitude = std::ldexp(float(mantissa | 0x400), int(exponent) - 25);
    return (half & 0x8000) ? -magnitude : magnitude;
}

std::array<uint32_t, touch_constant_words> pack_touch_overlay(const TouchOverlay& overlay) {
    const auto pair = [](float low, float high) {
        return uint32_t(float_to_half(low)) | (uint32_t(float_to_half(high)) << 16);
    };
    std::array<uint32_t, touch_constant_words> words{};
    words[0] = pair(overlay.knob[0], overlay.knob[1]);
    words[1] = pair(overlay.right_knob[0], overlay.right_knob[1]);
    for (size_t i = 0; i < TouchOverlay::circles; ++i) {
        const auto& c = overlay.circle[i];
        words[2 + i * 2] = pair(c[0], c[1]);
        words[3 + i * 2] = pair(c[2], c[3]);
    }
    return words;
}

void publish_touch_controls(const GamepadState& state, const TouchOverlay& overlay) {
    std::lock_guard lock(published_mutex);
    published_state = state;
    published_overlay = overlay;
}

GamepadState touch_gamepad() {
    std::lock_guard lock(published_mutex);
    return published_state;
}

void set_touch_racing(bool racing) { racing_now.store(racing, std::memory_order_relaxed); }
namespace { std::atomic<float> pause_progress{0.0f}; }
void publish_pause_gauge(float progress) { pause_progress.store(progress, std::memory_order_relaxed); }
float pause_gauge() { return pause_progress.load(std::memory_order_relaxed); }
bool touch_racing() { return racing_now.load(std::memory_order_relaxed); }

TouchOverlay touch_overlay() {
    std::lock_guard lock(published_mutex);
    return published_overlay;
}

namespace { std::atomic<bool> controller_present{false}, touched_last{false}; }

void note_controller(bool present) { controller_present.store(present, std::memory_order_relaxed); }

void note_controller_input() { touched_last.store(false, std::memory_order_relaxed); }

void note_touch() { touched_last.store(true, std::memory_order_relaxed); }

bool touch_controls_active() {
    return touch_controls_enabled() && (!controller_present.load(std::memory_order_relaxed) ||
                                        touched_last.load(std::memory_order_relaxed));
}

bool touch_controls_enabled() {
    static const bool enabled = [] {
        const char* text = std::getenv("SFR_TOUCH_CONTROLS");
#ifdef __ANDROID__
        return !text || *text != '0';
#else
        return text && *text == '1';
#endif
    }();
    return enabled;
}
}
