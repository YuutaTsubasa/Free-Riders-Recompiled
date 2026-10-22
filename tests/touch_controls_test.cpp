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

// Issue #217: outside races the screen is the hand cursor, and on the title
// screen a touch is START.
void pointer() {
    using sfr::TouchScene;
    TouchControls controls;
    controls.update(std::vector<TouchPoint>{{1, 0.6f, 0.3f}}, 0.0f, TouchScene::menu);
    require(controls.state() == sfr::GamepadState{} && controls.pointer() &&
            (*controls.pointer())[0] == 0.6f && (*controls.pointer())[1] == 0.3f,
            "an empty spot in the menus is the pointer");
    controls.update(std::vector<TouchPoint>{{1, 0.7f, 0.35f}}, 0.0f, TouchScene::menu);
    require((*controls.pointer())[0] == 0.7f, "the pointer follows its finger");
    controls.update(std::vector<TouchPoint>{at(1, TouchControls::a)}, 0.0f, TouchScene::menu);
    require(!controls.state().buttons && controls.pointer(), "the pointer's finger crosses buttons without pressing them");
    controls.update(std::vector<TouchPoint>{at(1, TouchControls::a), at(2, TouchControls::b)}, 0.0f, TouchScene::menu);
    require(controls.state().buttons == button::b, "the buttons work beside the pointer");
    controls.update(std::vector<TouchPoint>{{1, -0.2f, 1.3f}}, 0.0f, TouchScene::menu);
    require((*controls.pointer())[0] == 0.0f && (*controls.pointer())[1] == 1.0f, "the pointer stays on the image");
    controls.update({}, 0.0f, TouchScene::menu);
    require(!controls.pointer(), "lifting the finger ends the pointer");

    // The stick's side of the screen is the pointer too, except the stick's own circle.
    controls.update(std::vector<TouchPoint>{{3, 0.05f, 0.2f}}, 0.0f, TouchScene::menu);
    require(controls.pointer() && controls.overlay().circle[TouchControls::stick][3] == 1.0f,
            "the upper left is the pointer in the menus");
    controls.update({}, 0.0f, TouchScene::menu);
    controls.update(std::vector<TouchPoint>{at(4, TouchControls::stick)}, 0.0f, TouchScene::menu);
    controls.update(std::vector<TouchPoint>{at(4, TouchControls::stick, 0.1f)}, 0.0f, TouchScene::menu);
    require(!controls.pointer() && controls.state().buttons == button::dpad_right, "the stick's circle is still the D-pad");
    controls.update({}, 0.0f, TouchScene::menu);
    // A race has no pointer: the left side is the stick, the right side nothing.
    controls.update(std::vector<TouchPoint>{{5, 0.05f, 0.2f}, {6, 0.6f, 0.3f}}, 0.0f, TouchScene::race);
    require(!controls.pointer() && controls.overlay().circle[TouchControls::stick][3] == 2.0f, "no pointer in a race");
    controls.update({}, 0.0f, TouchScene::race);

    // The title screen: anywhere away from the controls is START.
    controls.update(std::vector<TouchPoint>{{7, 0.5f, 0.75f}}, 0.0f, TouchScene::title);
    require(controls.state().buttons == button::start && !controls.pointer(), "a touch on the title is START");
    require(controls.overlay().circle[TouchControls::start][3] == 2.0f, "START is drawn pressed");
    controls.update({}, 0.0f, TouchScene::title);
    require(!controls.state().buttons, "lifting the finger releases START");
    controls.update(std::vector<TouchPoint>{at(8, TouchControls::b)}, 0.0f, TouchScene::title);
    require(controls.state().buttons == button::b, "a button on the title is that button");
    controls.update({}, 0.0f, TouchScene::title);

    // The published target: each new position, and each new touch, is news.
    const uint32_t before = sfr::pointer_target().serial;
    sfr::publish_pointer(0.25f, 0.5f, true);
    const auto first = sfr::pointer_target();
    require(first.serial != before && first.x == 0.25f && first.y == 0.5f, "a pointer is published");
    sfr::publish_pointer(0.25f, 0.5f);
    require(sfr::pointer_target().serial == first.serial, "a resting pointer is not news");
    sfr::publish_pointer(0.25f, 0.5f, true);
    require(sfr::pointer_target().serial != first.serial, "a new touch on the same spot is news");
    // The mouse: window fractions through the blit's viewport.
    sfr::set_pointer_view(160.0f, 0.0f, 1600.0f, 900.0f, 1920.0f, 900.0f);
    const auto inside = sfr::pointer_from_window(960.0f / 1920.0f, 450.0f / 900.0f);
    require(inside && std::fabs((*inside)[0] - 0.5f) < 1e-6f && std::fabs((*inside)[1] - 0.5f) < 1e-6f,
            "the window's middle is the image's");
    require(!sfr::pointer_from_window(100.0f / 1920.0f, 0.5f), "the black bars are not the image");
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

// "Tilt to lean" (Issue #247): the phone's pitch leans forward / back.
void tilt_lean() {
    using sfr::tilt_pitch_degrees;
    const auto near = [](float value, float expected, float slack = 0.05f) { return std::abs(value - expected) <= slack; };
    const float rad = 3.14159265f / 180.0f;
    require(near(tilt_pitch_degrees(9.8f, 0.0f, 0.0f, 1.0f), 0.0f), "upright facing the player is pitch 0");
    require(near(tilt_pitch_degrees(0.0f, 0.0f, 9.8f, 1.0f), 90.0f), "lying face up is 90");
    require(near(tilt_pitch_degrees(std::cos(40 * rad), 0.0f, std::sin(40 * rad), 1.0f), 40.0f),
            "the top edge tipped away 40 degrees is 40");
    require(near(tilt_pitch_degrees(-std::cos(40 * rad), 0.0f, std::sin(40 * rad), -1.0f), 40.0f),
            "flipped landscape reads the same pitch");
    require(near(tilt_pitch_degrees(std::cos(40 * rad) * std::cos(20 * rad), std::cos(40 * rad) * std::sin(20 * rad),
                                    std::sin(40 * rad), 1.0f), 40.0f),
            "steering (turning the phone like a wheel) leaves the pitch alone");
    require(near(tilt_pitch_degrees(-std::sin(10 * rad), 0.0f, std::cos(10 * rad), 1.0f), 100.0f),
            "past lying flat the pitch goes on rising");

    const float frame = 1.0f / 60.0f;
    sfr::TiltLean lean;
    require(lean.update(40.0f, true, false, false, frame) == 0.0f && !lean.calibrated(), "nothing outside a race");
    require(lean.update(40.0f, true, true, false, frame) == 0.0f && lean.calibrated() && lean.neutral() == 40.0f,
            "the pitch when a race starts is neutral");
    require(lean.update(44.0f, true, true, false, frame) == 0.0f, "within 5 degrees of neutral is no lean");
    lean = {};
    lean.update(40.0f, true, true, false, frame);
    require(near(lean.update(55.0f, true, true, false, frame), 0.5f, 0.01f), "the top edge 15 degrees away is half a lean forward");
    require(lean.update(70.0f, true, true, false, frame) == 1.0f, "25 degrees and more is a full lean");
    require(near(lean.update(25.0f, true, true, false, frame), -0.5f, 0.02f), "towards the player leans back");
    // A lean held down a straight hardly fades; a grip shifting within the
    // dead zone becomes the neutral within a few seconds.
    lean = {};
    lean.update(40.0f, true, true, false, frame);
    float held = 0.0f;
    for (int i = 0; i < 180; ++i) held = lean.update(55.0f, true, true, false, frame);
    require(held > 0.4f, "a lean held for three seconds stays");
    lean = {};
    lean.update(40.0f, true, true, false, frame);
    for (int i = 0; i < 300; ++i) lean.update(44.0f, true, true, false, frame);
    require(near(lean.neutral(), 44.0f, 0.1f), "a small change of grip re-centres");
    require(near(lean.update(59.0f, true, true, false, frame), 0.5f, 0.02f), "and the lean is from the new neutral");
    // A, then the takeoff: no lean, so the stick does not turn into a trick.
    require(lean.update(64.0f, true, true, true, frame) == 0.0f, "no lean while A is held");
    float after = 1.0f;
    for (int i = 0; i < 24; ++i) after = lean.update(64.0f, true, true, false, frame);
    require(after == 0.0f, "nor for the takeoff after A is released");
    for (int i = 0; i < 12; ++i) after = lean.update(64.0f, true, true, false, frame);
    require(after > 0.5f, "then the lean is back");
    // Out of the race (or no landscape reading) the next race calibrates again.
    lean.update(64.0f, true, false, false, frame);
    require(!lean.calibrated(), "leaving the race forgets the neutral");
    lean.update(30.0f, false, true, false, frame);
    require(!lean.calibrated(), "no reading, no neutral");
    lean.update(30.0f, true, true, false, frame);
    require(lean.neutral() == 30.0f, "the next race takes its own neutral");

    // The touch controls: the lean is the left stick's Y in a race only, and
    // the on-screen stick takes both axes over.
    TouchControls controls;
    controls.update({}, 0.0f, sfr::TouchScene::race, 0.5f);
    require(controls.state().thumb_ly > 16000 && controls.state().thumb_ly < 16500 && controls.state().thumb_lx == 0,
            "tilt lean is the left stick's Y in a race");
    controls.update({}, -0.5f, sfr::TouchScene::race, -1.0f);
    require(controls.state().thumb_ly == -32767 && controls.state().thumb_lx < -16000, "steering and leaning together");
    controls.update({}, 0.0f, sfr::TouchScene::menu, 1.0f);
    require(controls.state().thumb_ly == 0, "tilt lean does nothing in the menus");
    controls.update(std::vector<TouchPoint>{{8, 0.2f, 0.7f}}, -0.5f, sfr::TouchScene::race, 1.0f);
    require(controls.state().thumb_ly == 0 && controls.state().thumb_lx == 0, "the stick overrides the tilt lean");
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
        pointer();
        packing();
        controller_handover();
        tilt_lean();
        std::cout << "Touch control checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
