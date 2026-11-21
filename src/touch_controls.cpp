#include "touch_controls.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <mutex>

namespace sfr {
static_assert(touch_options::names.size() == TouchOverlay::circles);
static_assert(touch_options::bit_of("ls") == 1u << TouchControls::stick && touch_options::bit_of("A") == 1u << TouchControls::a &&
                  touch_options::bit_of("rt") == 1u << TouchControls::trigger &&
                  touch_options::bit_of("start") == 1u << TouchControls::start &&
                  touch_options::bit_of("lb") == 1u << TouchControls::lb &&
                  touch_options::bit_of("rs") == 1u << TouchControls::right_stick,
              "touch_options::names follows TouchControls::Button");
namespace {
using touch_layout::touch_slack;  // a button reacts a little outside its circle

// Distance in image heights, in an image aspect times as wide as it is tall.
float distance(float x0, float y0, float x1, float y1, float aspect) {
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

float touch_anchor_x(float x, float aspect) { return touch_layout::anchor_x(x, aspect); }

float touch_image_aspect(float width, float height) {
    if (!(width > 0.0f) || !(height > 0.0f)) return touch_layout_aspect;
    return std::clamp(width / height, 0.5f, 16.0f);
}

std::array<float, 3> TouchControls::layout(Button button, float aspect) {
    const auto [left, top, radius] = layout_16x9(button);
    return {touch_anchor_x(left, aspect), top, radius};
}

std::array<float, 3> TouchControls::layout_16x9(Button button) {
    return button < touch_layout::count ? touch_layout::defaults[button] : std::array<float, 3>{};
}

bool TouchControls::in_race_stick_area(float x) const {
    // The stick's side: the left 40% (the right 40% once LS is put on the
    // right half), reaching further in when LS's circle does.
    const auto [from, to] = touch_layout::race_stick_area(layout_.places[stick], aspect_);
    return from == 0.0f ? x >= 0.0f && x < to : x > from && x <= 1.0f;
}

namespace {
// Where a finger on a stick puts its knob: (dx, dy) in -1..1 from the base.
std::array<float, 2> stick_offset(const TouchPoint& finger, std::array<float, 2> base, float radius, float aspect) {
    float dx = (finger.x - base[0]) * aspect / radius;
    float dy = (finger.y - base[1]) / radius;
    const float length = std::hypot(dx, dy);
    if (length > 1.0f) { dx /= length; dy /= length; }
    return {dx, dy};
}
}

void TouchControls::update(std::span<const TouchPoint> touches, float tilt, TouchScene scene, bool kick) {
    namespace button = gamepad_button;
    const bool racing = scene == TouchScene::race;
    state_ = {};
    pressed_.fill(false);
    hidden_ = scene == TouchScene::scene;
    if (hidden_) {
        // Nothing is drawn, so nothing is held: the sticks let go, and a
        // finger anywhere is START until it lifts (the title's pointer).
        stick_active_ = false;
        stick_finger_ = right_finger_ = -1;
        right_knob_ = {place(right_stick)[0], place(right_stick)[1]};
        pointer_.reset();
        const auto held = std::find_if(touches.begin(), touches.end(),
                                       [&](const TouchPoint& t) { return t.id == pointer_finger_; });
        if (held == touches.end()) pointer_finger_ = touches.empty() ? -1 : touches.front().id;
        if (pointer_finger_ >= 0) {
            pressed_[start] = true;
            state_.buttons |= button::start;
        }
        return;
    }
    // Each stick follows the finger that started it until that finger lifts.
    const auto held = [&](int64_t finger) {
        return std::find_if(touches.begin(), touches.end(), [&](const TouchPoint& t) { return t.id == finger; });
    };
    if (held(stick_finger_) == touches.end()) {
        stick_active_ = false;
        stick_finger_ = -1;
    }
    if (held(right_finger_) == touches.end()) right_finger_ = -1;
    // The pointer belongs to the scene it started in: a race takes it away.
    if (held(pointer_finger_) == touches.end() || racing) pointer_finger_ = -1;
    const float aspect = aspect_;
    const auto [right_x, right_y, right_radius] = place(right_stick);
    const auto [stick_x, stick_y, stick_radius] = place(stick);
    // A hidden control is empty screen (TouchOptions).
    const bool stick_shown = options_.shown(stick), right_shown = options_.shown(right_stick);
    for (const TouchPoint& touch : touches) {
        if (touch.id == stick_finger_ || touch.id == right_finger_ || touch.id == pointer_finger_) continue;
        // Each button whose circle or slack the touch is in (a thumb across
        // two buttons), as always; but inside two or more circles, which
        // only a player's layout puts over each other (the default's never
        // touch), the nearest one (in radii) only, so each keeps its middle.
        size_t nearest = 0;
        float nearest_by = 2.0f;
        int inside = 0;
        uint32_t slack = 0;
        for (size_t index = a; index <= lt; ++index) {
            if (!options_.shown(index)) continue;
            const auto [bx, by, br] = place(Button(index));
            const float by_radii = distance(bx, by, touch.x, touch.y, aspect) / br;
            if (by_radii <= 1.0f) {
                ++inside;
                if (by_radii < nearest_by) {
                    nearest = index;
                    nearest_by = by_radii;
                }
            }
            if (by_radii <= touch_slack) slack |= 1u << index;
        }
        if (inside >= 2) {
            pressed_[nearest] = true;
            continue;
        }
        if (slack) {
            for (size_t index = a; index <= lt; ++index)
                if (slack & (1u << index)) pressed_[index] = true;
            continue;
        }
        // Outside a race the stick starts only on its own circle: the rest
        // of the screen is the pointer.
        const bool stick_area = stick_shown &&
                                (racing ? in_race_stick_area(touch.x)
                                        : distance(stick_x, stick_y, touch.x, touch.y, aspect) <= stick_radius * touch_slack);
        if (right_shown && right_finger_ < 0 && distance(right_x, right_y, touch.x, touch.y, aspect) <= right_radius * touch_slack) {
            right_finger_ = touch.id;
        } else if (!stick_active_ && stick_area) {
            stick_active_ = true;
            stick_finger_ = touch.id;
            stick_base_ = {touch.x, std::clamp(touch.y, stick_radius, 1.0f - stick_radius)};
        } else if (!racing && pointer_finger_ < 0 && !stick_area) {
            pointer_finger_ = touch.id;
        }
    }
    pointer_.reset();
    if (pointer_finger_ >= 0) {
        if (scene == TouchScene::title) {
            pressed_[start] = true;  // the title's START, wherever it is touched
        } else {
            const TouchPoint& finger = *held(pointer_finger_);
            pointer_ = std::array<float, 2>{std::clamp(finger.x, 0.0f, 1.0f), std::clamp(finger.y, 0.0f, 1.0f)};
        }
    }
    if (stick_active_) {
        const auto [dx, dy] = stick_offset(*held(stick_finger_), stick_base_, stick_radius, aspect);
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
    } else if (racing) {
        state_.thumb_lx = axis(tilt);
    }
    if (racing && kick) pressed_[x] = true;  // a tilt kick dash, drawn as X held
    right_knob_ = {right_x, right_y};
    if (right_finger_ >= 0) {
        // The right stick is analog everywhere: the race's reach and arms up,
        // the menus' hand cursor. The race's own dead zone ignores a resting
        // thumb's wobble.
        const auto [dx, dy] = stick_offset(*held(right_finger_), {right_x, right_y}, right_radius, aspect);
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
    if (hidden_) return overlay;  // every circle at state 0
    for (size_t index = 0; index < TouchOverlay::circles; ++index) {
        auto [cx, cy, radius] = place(Button(index));
        if (index == stick && stick_active_) { cx = stick_base_[0]; cy = stick_base_[1]; }
        // Hidden: state 0, nothing drawn; without its name: negative.
        const float state = !options_.shown(index) ? 0.0f : pressed_[index] ? 2.0f : 1.0f;
        overlay.circle[index] = {cx, cy, radius, options_.named(index) ? state : -state};
    }
    overlay.knob = stick_active_ ? stick_knob_ : std::array<float, 2>{place(stick)[0], place(stick)[1]};
    // At rest (also before the first update) the right knob is on its base.
    overlay.right_knob = right_finger_ >= 0 ? right_knob_
                                           : std::array<float, 2>{place(right_stick)[0], place(right_stick)[1]};
    return overlay;
}

float tilt_pitch_degrees(float x, float y, float z, float side) {
    // Upright, gravity's reaction lies along the landscape's up (the device's
    // +x, -x flipped); face up, along z. The steering's roll moves it into y,
    // so y counts with up, keeping up's sign (past flat it turns negative).
    const float up = side * x;
    return std::atan2(z, std::copysign(std::hypot(up, y), up)) * 57.29578f;
}

float TiltLean::update(float pitch, bool valid, bool racing, bool jumping, float seconds) {
    if (!racing || !valid) {
        // A new race (or the phone back in landscape) takes a new neutral.
        calibrated_ = false;
        quiet_ = 0.0f;
        return 0.0f;
    }
    if (!calibrated_) {
        calibrated_ = true;
        neutral_ = pitch;
    }
    seconds = std::clamp(seconds, 0.0f, 0.25f);
    const float offset = pitch - neutral_;
    // Within the dead zone the neutral follows in about a second, beyond it
    // in about half a minute.
    const float follow = std::abs(offset) < dead_zone ? 1.0f : 30.0f;
    neutral_ += offset * (1.0f - std::exp(-seconds / follow));
    if (jumping) quiet_ = after_jump;
    else quiet_ = std::max(quiet_ - seconds, 0.0f);
    if (jumping || quiet_ > 0.0f) return 0.0f;
    const float beyond = std::max(std::abs(offset) - dead_zone, 0.0f) / (full - dead_zone);
    return std::copysign(std::min(beyond, 1.0f), offset);
}

bool TiltKick::update(float lean, float seconds) {
    held_ = std::max(held_ - std::max(seconds, 0.0f), 0.0f);
    if (armed_ && lean >= trigger) {
        armed_ = false;
        held_ = press;
    } else if (!armed_ && lean <= rearm) {
        armed_ = true;
    }
    return held_ > 0.0f;
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
    words[touch_readout_word] = overlay.readout;
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

namespace {
std::atomic<bool> title_now{false};
std::mutex pointer_mutex;
PointerTarget pointer_now;
std::array<float, 6> pointer_view{};
}

void set_touch_title(bool waiting) { title_now.store(waiting, std::memory_order_relaxed); }

namespace {
std::atomic<int64_t> movie_ms{-1000000};
std::atomic<bool> story_now{false};
int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

void note_touch_movie() { movie_ms.store(steady_ms(), std::memory_order_relaxed); }
void set_touch_story(bool on) { story_now.store(on, std::memory_order_relaxed); }

bool touch_in_scenes() {
    static const bool shown = [] {
        const char* text = std::getenv("SFR_TOUCH_IN_SCENES");
        return text && *text == '1';
    }();
    return shown;
}

TouchScene touch_scene_for(bool racing, bool movie, bool title, bool story, bool show_in_scenes) {
    if (racing) return TouchScene::race;
    // The opening movie (titleMovie, the disc's only one) plays inside the
    // title's states: there it is a scene, and a touch is START either way.
    if (movie && !show_in_scenes) return TouchScene::scene;
    if (title) return TouchScene::title;
    if (story && !show_in_scenes) return TouchScene::scene;
    return TouchScene::menu;
}

TouchScene touch_scene() {
    // A movie plays at most a few frames apart; a quarter of a second without
    // one is its end.
    const bool movie = steady_ms() - movie_ms.load(std::memory_order_relaxed) < 250;
    return touch_scene_for(touch_racing(), movie, title_now.load(std::memory_order_relaxed),
                           story_now.load(std::memory_order_relaxed), touch_in_scenes());
}

void publish_pointer(float x, float y, bool landed, bool held) {
    std::lock_guard lock(pointer_mutex);
    pointer_now.held = held;
    if (!landed && pointer_now.serial && pointer_now.x == x && pointer_now.y == y) return;  // resting: nothing new
    pointer_now.x = x;
    pointer_now.y = y;
    if (++pointer_now.serial == 0) pointer_now.serial = 1;
}

void release_pointer() {
    std::lock_guard lock(pointer_mutex);
    pointer_now.held = false;
}

PointerTarget pointer_target() {
    std::lock_guard lock(pointer_mutex);
    return pointer_now;
}

void set_pointer_view(float x, float y, float width, float height, float out_width, float out_height) {
    std::lock_guard lock(pointer_mutex);
    pointer_view = {x, y, width, height, out_width, out_height};
}

std::optional<std::array<float, 2>> pointer_from_window(float x, float y) {
    std::lock_guard lock(pointer_mutex);
    const auto [view_x, view_y, view_width, view_height, out_width, out_height] = pointer_view;
    if (view_width <= 0.0f || view_height <= 0.0f) return std::nullopt;
    const float u = (x * out_width - view_x) / view_width, v = (y * out_height - view_y) / view_height;
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return std::nullopt;
    return std::array<float, 2>{u, v};
}

TouchOverlay touch_overlay() {
    std::lock_guard lock(published_mutex);
    return published_overlay;
}

TouchOverlay touch_overlay_at_rest(float aspect) {
    TouchControls controls;
    controls.set_aspect(aspect);
    controls.set_options(touch_options_chosen());
    controls.set_layout(touch_layout_chosen());
    return controls.overlay();
}

namespace { std::atomic<bool> controller_present{false}, touched_last{false}; }

void note_controller(bool present) { controller_present.store(present, std::memory_order_relaxed); }

void note_controller_input() { touched_last.store(false, std::memory_order_relaxed); }

void note_touch() { touched_last.store(true, std::memory_order_relaxed); }

bool touch_controls_active() {
    return touch_controls_enabled() && (!controller_present.load(std::memory_order_relaxed) ||
                                        touched_last.load(std::memory_order_relaxed));
}

bool touch_names_shown() {
    static const bool shown = [] {
        const char* text = std::getenv("SFR_TOUCH_LABELS");
        return !text || *text != '0';
    }();
    return shown;
}

TouchOptions touch_options_from(const char* hidden, const char* unnamed) {
    TouchOptions options;
    if (hidden) options.hidden = touch_options::parse(hidden);
    if (unnamed) options.unnamed = touch_options::parse(unnamed);
    return options;
}

TouchOptions touch_options_chosen() {
    static const TouchOptions chosen = [] {
        const TouchOptions options = touch_options_from(std::getenv("SFR_TOUCH_HIDDEN"), std::getenv("SFR_TOUCH_UNNAMED"));
        if (options.hidden || options.unnamed)
            std::fprintf(stderr, "TOUCH options hidden=%s unnamed=%s\n", touch_options::format(options.hidden).c_str(),
                         touch_options::format(options.unnamed).c_str());
        return options;
    }();
    return chosen;
}

TouchLayout touch_layout_chosen() {
    static const TouchLayout chosen = [] {
        const char* text = std::getenv("SFR_TOUCH_LAYOUT");
        const TouchLayout layout = text ? touch_layout::parse(text) : TouchLayout{};
        if (!layout.empty()) std::fprintf(stderr, "TOUCH layout %s\n", touch_layout::format(layout).c_str());
        return layout;
    }();
    return chosen;
}

bool touch_controls_enabled() {
    static const bool enabled = [] {
        const char* text = std::getenv("SFR_TOUCH_CONTROLS");
#if defined(__ANDROID__) || defined(SFR_IOS)
        return !text || *text != '0';
#else
        return text && *text == '1';
#endif
    }();
    return enabled;
}
}
