#include "touch_controls.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
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
TouchOverlay published_overlay;
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
    }
    return {};
}

void TouchControls::update(std::span<const TouchPoint> touches, float tilt, bool racing) {
    namespace button = gamepad_button;
    state_ = {};
    pressed_.fill(false);
    // The stick follows the finger that started it until that finger lifts.
    const auto stick_touch = std::find_if(touches.begin(), touches.end(),
                                          [&](const TouchPoint& t) { return t.id == stick_finger_; });
    if (stick_touch == touches.end()) {
        stick_active_ = false;
        stick_finger_ = -1;
    }
    const float stick_radius = layout(stick)[2];
    for (const TouchPoint& touch : touches) {
        if (touch.id == stick_finger_) continue;
        bool on_button = false;
        for (size_t index = a; index <= start; ++index) {
            const auto [bx, by, br] = layout(Button(index));
            if (distance(bx, by, touch.x, touch.y) <= br * touch_slack) {
                pressed_[index] = true;
                on_button = true;
            }
        }
        if (!on_button && !stick_active_ && touch.x < stick_region && touch.x >= 0.0f) {
            stick_active_ = true;
            stick_finger_ = touch.id;
            stick_base_ = {touch.x, std::clamp(touch.y, stick_radius, 1.0f - stick_radius)};
        }
    }
    if (stick_active_) {
        const auto finger = std::find_if(touches.begin(), touches.end(),
                                         [&](const TouchPoint& t) { return t.id == stick_finger_; });
        float dx = (finger->x - stick_base_[0]) * aspect / stick_radius;
        float dy = (finger->y - stick_base_[1]) / stick_radius;
        const float length = std::hypot(dx, dy);
        if (length > 1.0f) { dx /= length; dy /= length; }
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
    if (pressed_[a]) state_.buttons |= button::a;
    if (pressed_[b]) state_.buttons |= button::b;
    if (pressed_[x]) state_.buttons |= button::x;
    if (pressed_[y]) state_.buttons |= button::y;
    if (pressed_[start]) state_.buttons |= button::start;
    if (pressed_[trigger]) state_.right_trigger = 255;
}

TouchOverlay TouchControls::overlay() const {
    TouchOverlay overlay;
    for (size_t index = 0; index < TouchOverlay::circles; ++index) {
        auto [cx, cy, radius] = layout(Button(index));
        if (index == stick && stick_active_) { cx = stick_base_[0]; cy = stick_base_[1]; }
        overlay.circle[index] = {cx, cy, radius, pressed_[index] ? 2.0f : 1.0f};
    }
    overlay.knob = stick_active_ ? stick_knob_ : std::array<float, 2>{layout(stick)[0], layout(stick)[1]};
    return overlay;
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
bool touch_racing() { return racing_now.load(std::memory_order_relaxed); }

TouchOverlay touch_overlay() {
    std::lock_guard lock(published_mutex);
    return published_overlay;
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
