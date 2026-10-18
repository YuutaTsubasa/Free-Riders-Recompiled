#include "touch_controls.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool ok, const char* detail) { if (!ok) throw std::runtime_error(detail); }
using sfr::TouchControls;
using sfr::TouchPoint;
namespace button = sfr::gamepad_button;

TouchPoint at(int64_t id, TouchControls::Button control, float dx = 0.0f, float dy = 0.0f) {
    const auto [x, y, r] = TouchControls::layout(control);
    return {id, x + dx, y + dy};
}

void run() {
    TouchControls controls;
    controls.update({});
    require(controls.state() == sfr::GamepadState{}, "no touch is rest");
    require(controls.overlay().circle[TouchControls::a][3] == 1.0f, "buttons are shown when idle");

    const std::vector<TouchPoint> a_and_start{at(1, TouchControls::a), at(2, TouchControls::start)};
    controls.update(a_and_start);
    require(controls.state().buttons == (button::a | button::start), "touching A and START presses both");
    require(controls.overlay().circle[TouchControls::a][3] == 2.0f, "a pressed button is drawn pressed");

    controls.update(std::vector<TouchPoint>{at(3, TouchControls::trigger)});
    require(controls.state().right_trigger == 255 && !controls.state().buttons, "the trigger button is RT");

    // The stick in a race: its base appears where the finger lands, the knob follows.
    controls.update(std::vector<TouchPoint>{{4, 0.20f, 0.60f}}, 0.0f, true);
    require(controls.state().thumb_lx == 0 && controls.state().thumb_ly == 0, "a fresh stick is centred");
    controls.update(std::vector<TouchPoint>{{4, 0.30f, 0.60f}}, 0.0f, true);
    require(controls.state().thumb_lx == 32767 && !(controls.state().buttons & button::dpad_right),
            "in a race dragging right is the left stick");
    controls.update(std::vector<TouchPoint>{{4, 0.20f, 0.55f}}, 0.0f, true);
    require(controls.state().thumb_ly > 0 && controls.state().thumb_lx == 0, "a small drag up is up on the stick");
    // The stick finger may cross a button without pressing it.
    controls.update(std::vector<TouchPoint>{at(4, TouchControls::x)}, 0.0f, true);
    require(!(controls.state().buttons & button::x), "the stick finger does not press buttons");
    controls.update({}, 0.0f, true);
    require(controls.state() == sfr::GamepadState{}, "lifting the finger centres the stick");

    // In the menus the same drag is the D-pad, without the analog stick.
    controls.update(std::vector<TouchPoint>{{7, 0.20f, 0.60f}});
    controls.update(std::vector<TouchPoint>{{7, 0.30f, 0.60f}});
    require(controls.state().buttons == button::dpad_right && controls.state().thumb_lx == 0,
            "in the menus dragging right is D-pad right only");
    controls.update(std::vector<TouchPoint>{{7, 0.21f, 0.60f}});
    require(controls.state() == sfr::GamepadState{}, "a small drag in the menus does nothing");
    controls.update({});

    controls.update({}, -0.5f, true);
    require(controls.state().thumb_lx < -16000 && controls.state().thumb_lx > -16500, "tilt steers in a race");
    controls.update({}, -0.5f);
    require(controls.state().thumb_lx == 0, "tilt does nothing in the menus");
    controls.update(std::vector<TouchPoint>{{5, 0.2f, 0.7f}}, -0.5f, true);
    require(controls.state().thumb_lx == 0, "the stick overrides the tilt");
    controls.update(std::vector<TouchPoint>{{6, 0.6f, 0.3f}});
    require(controls.state() == sfr::GamepadState{}, "the empty right side does nothing");
}

// Issue #177: the shoulder buttons, LT and the right stick, so that every
// race action a pad has is on the screen.
void race_actions() {
    TouchControls controls;
    controls.update(std::vector<TouchPoint>{at(1, TouchControls::lb)}, 0.0f, true);
    require(controls.state().buttons == button::left_shoulder, "LB is the left punch");
    controls.update(std::vector<TouchPoint>{at(2, TouchControls::rb)}, 0.0f, true);
    require(controls.state().buttons == button::right_shoulder, "RB is the right punch");
    require(controls.overlay().circle[TouchControls::rb][3] == 2.0f, "a pressed RB is drawn pressed");
    controls.update(std::vector<TouchPoint>{at(3, TouchControls::lt)}, 0.0f, true);
    require(controls.state().left_trigger == 255 && !controls.state().buttons, "LT holds hands");
    controls.update({}, 0.0f, true);

    // The right stick: a fixed base, the knob follows the finger that started on it.
    controls.update(std::vector<TouchPoint>{at(4, TouchControls::right_stick)}, 0.0f, true);
    require(controls.state() == sfr::GamepadState{}, "a finger at the right stick's centre is at rest");
    require(controls.overlay().circle[TouchControls::right_stick][3] == 2.0f, "a held right stick is drawn pressed");
    const float right_radius = TouchControls::layout(TouchControls::right_stick)[2];
    controls.update(std::vector<TouchPoint>{at(4, TouchControls::right_stick, -right_radius * 2.0f)}, 0.0f, true);
    require(controls.state().thumb_rx == -32767 && controls.state().thumb_ry == 0,
            "dragging left is a full reach to the left");
    const auto right = TouchControls::layout(TouchControls::right_stick);
    const auto knob = controls.overlay().right_knob;
    require(std::fabs(knob[0] - (right[0] - right_radius * 9.0f / 16.0f)) < 1e-4f && std::fabs(knob[1] - right[1]) < 1e-4f,
            "the right knob stops at the base's rim");
    controls.update(std::vector<TouchPoint>{at(4, TouchControls::right_stick, 0.0f, -right_radius * 0.5f)}, 0.0f, true);
    require(controls.state().thumb_ry > 16000 && controls.state().thumb_ry < 16500 && controls.state().thumb_rx == 0,
            "half way up raises the arms half way");
    // Its finger crosses buttons without pressing them, and the left stick works alongside.
    controls.update(std::vector<TouchPoint>{at(4, TouchControls::y), {5, 0.20f, 0.60f}}, 0.0f, true);
    require(!(controls.state().buttons & button::y), "the right stick's finger does not press buttons");
    controls.update(std::vector<TouchPoint>{at(4, TouchControls::right_stick, right_radius), {5, 0.30f, 0.60f}}, 0.0f, true);
    require(controls.state().thumb_rx == 32767 && controls.state().thumb_lx == 32767, "both sticks at once");
    controls.update({}, 0.0f, true);
    require(controls.state() == sfr::GamepadState{}, "lifting the fingers centres both sticks");
    require(controls.overlay().right_knob[0] == right[0], "the right knob goes back to the centre");
    // In the menus it is the hand cursor: still analog.
    controls.update(std::vector<TouchPoint>{at(6, TouchControls::right_stick)});
    controls.update(std::vector<TouchPoint>{at(6, TouchControls::right_stick, right_radius)});
    require(controls.state().thumb_rx == 32767 && !controls.state().buttons, "the right stick is analog in the menus");
    controls.update({});

    // No two controls are drawn over each other, and a touch meant for one of
    // the new ones never reaches another (a touch between two diamond buttons
    // presses both, as it always has).
    for (size_t i = TouchControls::a; i < sfr::TouchOverlay::circles; ++i)
        for (size_t j = i + 1; j < sfr::TouchOverlay::circles; ++j) {
            const auto [x0, y0, r0] = TouchControls::layout(TouchControls::Button(i));
            const auto [x1, y1, r1] = TouchControls::layout(TouchControls::Button(j));
            const float apart = std::hypot((x1 - x0) * 16.0f / 9.0f, y1 - y0);
            require(apart > r0 + r1, "two controls overlap");
            if (j >= TouchControls::lb) require(apart > (r0 + r1) * 1.25f, "a new control's touch area overlaps another");
        }
    // Each stays on the 16:9 image and clear of the left stick's resting place.
    for (size_t i = TouchControls::a; i < sfr::TouchOverlay::circles; ++i) {
        const auto [x, y, r] = TouchControls::layout(TouchControls::Button(i));
        require(x - r * 9.0f / 16.0f >= 0.0f && x + r * 9.0f / 16.0f <= 1.0f && y - r >= 0.0f && y + r <= 1.0f,
                "a control leaves the image");
    }
}

// The blit's push constants: the overlay as half floats, 30 words after the area.
void packing() {
    for (float value : {0.0f, 0.065f, 0.44f, 0.935f, 1.0f, 2.0f, 3.5f, 4.0f, -0.25f})
        require(std::fabs(sfr::half_to_float(sfr::float_to_half(value)) - value) <= std::fabs(value) / 1024.0f,
                "a half float keeps the value");
    require(sfr::float_to_half(1.0f) == 0x3C00 && sfr::float_to_half(-2.0f) == 0xC000 && sfr::float_to_half(0.0f) == 0,
            "half float bit patterns");
    require(sfr::float_to_half(1e-6f) == 0x0011 && sfr::float_to_half(1e6f) == 0x7C00, "tiny and huge half floats");

    TouchControls controls;
    controls.update(std::vector<TouchPoint>{at(1, TouchControls::rb)});
    auto overlay = controls.overlay();
    overlay.circle[TouchControls::start] = {0.075f, 0.86f, 0.05f, 3.5f};
    const auto words = sfr::pack_touch_overlay(overlay);
    const auto low = [](uint32_t word) { return sfr::half_to_float(uint16_t(word & 0xFFFF)); };
    const auto high = [](uint32_t word) { return sfr::half_to_float(uint16_t(word >> 16)); };
    require(std::fabs(low(words[0]) - overlay.knob[0]) < 1e-3f && std::fabs(high(words[1]) - overlay.right_knob[1]) < 1e-3f,
            "the knobs lead");
    for (size_t i = 0; i < sfr::TouchOverlay::circles; ++i) {
        const auto& c = overlay.circle[i];
        const uint32_t xy = words[2 + i * 2], rs = words[3 + i * 2];
        require(std::fabs(low(xy) - c[0]) < 1e-3f && std::fabs(high(xy) - c[1]) < 1e-3f &&
                std::fabs(low(rs) - c[2]) < 1e-4f && std::fabs(high(rs) - c[3]) < 1e-3f, "a circle survives packing");
    }
    require(high(words[3 + TouchControls::rb * 2]) == 2.0f, "the pressed state is exact");
    require(words.size() + 2 == 32, "area and overlay fill 128 bytes");
}

// Issue #177: a touch brings the controls back over a connected controller,
// and the controller's input hides them again.
void controller_handover() {
    require(sfr::touch_controls_enabled(), "SFR_TOUCH_CONTROLS=1 turns the controls on");
    sfr::note_controller(false);
    require(sfr::touch_controls_active(), "without a controller the controls are on");
    sfr::note_controller(true);
    require(!sfr::touch_controls_active(), "a controller hides them");
    sfr::note_touch();
    require(sfr::touch_controls_active(), "a touch brings them back with a controller connected");
    sfr::note_controller(true);
    require(sfr::touch_controls_active(), "they stay while the controller does nothing");
    sfr::note_controller_input();
    require(!sfr::touch_controls_active(), "the controller's input hides them again");
    sfr::note_controller(false);
    require(sfr::touch_controls_active(), "unplugging the controller shows them");
}
}

int main() {
#ifdef _WIN32
    _putenv_s("SFR_TOUCH_CONTROLS", "1");
#else
    setenv("SFR_TOUCH_CONTROLS", "1", 1);
#endif
    try {
        run();
        race_actions();
        packing();
        controller_handover();
        std::cout << "Touch control checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
