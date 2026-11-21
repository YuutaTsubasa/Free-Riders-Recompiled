#include "touch_controls.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <utility>
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

    // Tilt to kick: a forward tip presses X once, for a moment.
    sfr::TiltKick kick;
    require(!kick.update(0.5f, frame), "below the trigger no kick");
    require(kick.update(0.7f, frame), "past the trigger: X");
    int held_frames = 1;
    while (kick.update(0.9f, frame)) ++held_frames;
    require(held_frames >= 8 && held_frames <= 10, "X is held for about 0.15 s");
    for (int i = 0; i < 60; ++i) require(!kick.update(1.0f, frame), "held tipped: only one kick");
    require(!kick.update(0.2f, frame), "back near neutral re-arms");
    require(kick.update(0.65f, frame), "and the next tip kicks again");
    sfr::TiltKick back;
    for (int i = 0; i < 30; ++i) require(!back.update(-1.0f, frame), "tipping back does nothing");

    // The touch controls: the kick is X in a race only, with or without the
    // on-screen stick, and the tilt leaves the stick's Y alone.
    TouchControls controls;
    controls.update({}, -0.5f, sfr::TouchScene::race, true);
    require((controls.state().buttons & sfr::gamepad_button::x) && controls.state().thumb_ly == 0 &&
                controls.state().thumb_lx < -16000,
            "a tilt kick is X in a race, beside steering");
    controls.update({}, 0.0f, sfr::TouchScene::menu, true);
    require(!(controls.state().buttons & sfr::gamepad_button::x), "no tilt kick in the menus");
    controls.update(std::vector<TouchPoint>{{8, 0.2f, 0.7f}}, 0.0f, sfr::TouchScene::race, true);
    require(controls.state().buttons & sfr::gamepad_button::x, "the kick works with the stick held");
}

// Issue #274: movies and story scenes draw no controls, and a touch anywhere
// is START while the finger stays down; held, it skips (Issue #426).
void scenes() {
    using sfr::TouchScene;
    TouchControls controls;
    controls.update({}, 0.0f, TouchScene::scene);
    for (const auto& circle : controls.overlay().circle) require(circle[3] == 0.0f, "a scene draws no controls");
    require(controls.state() == sfr::GamepadState{}, "a scene untouched presses nothing");
    controls.update(std::vector<TouchPoint>{at(1, TouchControls::a)}, 0.0f, TouchScene::scene);
    require(controls.state().buttons == button::start && !controls.pointer(), "a touch on A's place is START");
    controls.update(std::vector<TouchPoint>{at(1, TouchControls::a), {2, 0.1f, 0.7f}}, 0.0f, TouchScene::scene);
    require(controls.state().buttons == button::start && !controls.state().thumb_lx,
            "a second finger moves no stick");
    for (const auto& circle : controls.overlay().circle) require(circle[3] == 0.0f, "nothing is drawn pressed");
    controls.update({}, 0.0f, TouchScene::scene);
    require(!controls.state().buttons, "lifting the finger releases START");
    controls.update(std::vector<TouchPoint>{{3, 0.5f, 0.5f}}, 0.0f, TouchScene::scene);
    controls.update({}, 0.0f, TouchScene::menu);
    require(controls.overlay().circle[TouchControls::a][3] == 1.0f, "back in the menus the controls return");

    // Which scene: a race first, then a movie, the title, a story scene.
    require(sfr::touch_scene_for(true, true, true, true, false) == TouchScene::race, "a race is a race");
    require(sfr::touch_scene_for(false, true, true, false, false) == TouchScene::scene,
            "the opening movie inside the title's states is a scene");
    require(sfr::touch_scene_for(false, false, true, true, false) == TouchScene::title, "the title keeps its controls");
    require(sfr::touch_scene_for(false, false, false, true, false) == TouchScene::scene, "a story scene hides them");
    require(sfr::touch_scene_for(false, true, false, true, true) == TouchScene::menu &&
                sfr::touch_scene_for(false, true, true, false, true) == TouchScene::title,
            "SFR_TOUCH_IN_SCENES=1 keeps them");
    require(sfr::touch_scene_for(false, false, false, false, false) == TouchScene::menu, "otherwise the menus");
}

// Issue #443: with Aspect Ratio Auto a phone's picture fills its 20:9
// display, and the 16:9 layout was stretched across it (ovals about a fifth
// wider, and touches read the same stretched shapes). In an image of any
// shape each control keeps its size, its round shape and its distance from
// the nearer side edge, all in image heights.
void aspects() {
    using sfr::touch_layout_aspect;
    const float k16x9 = touch_layout_aspect;
    for (size_t i = 0; i < sfr::TouchOverlay::circles; ++i) {
        const auto button = TouchControls::Button(i);
        const auto plain = TouchControls::layout(button);
        const auto same = TouchControls::layout(button, k16x9);
        require(plain == same, "16:9 keeps the layout as it was");
    }
    require(sfr::touch_anchor_x(0.5f, 21.0f / 9.0f) == 0.5f, "the centre stays the centre");
    require(sfr::touch_image_aspect(0.0f, 1080.0f) == k16x9, "an unknown image is 16:9");
    require(std::fabs(sfr::touch_image_aspect(2340.0f, 1080.0f) - 19.5f / 9.0f) < 1e-5f, "the viewport gives the aspect");

    // Distance from the nearer side edge in image heights.
    const auto from_edge = [](float x, float aspect) { return (x < 0.5f ? x : 1.0f - x) * aspect; };
    const float shapes[] = {20.0f / 9.0f, 19.5f / 9.0f, 21.0f / 9.0f, 32.0f / 9.0f, 16.0f / 10.0f, 4.0f / 3.0f};
    for (const float aspect : shapes) {
        for (size_t i = 0; i < sfr::TouchOverlay::circles; ++i) {
            const auto button = TouchControls::Button(i);
            const auto [x0, y0, r0] = TouchControls::layout(button);
            const auto [x, y, r] = TouchControls::layout(button, aspect);
            require(y == y0 && r == r0, "a control keeps its height and size");
            if (x0 == 0.5f) require(x == 0.5f, "START stays in the middle");
            else require(std::fabs(from_edge(x, aspect) - from_edge(x0, k16x9)) < 1e-4f, "a control keeps its place from the edge");
            require(x - r / aspect >= 0.0f && x + r / aspect <= 1.0f, "a control leaves the image");
        }
        // The diamond keeps its shape: A to B as far as A to Y, in heights.
        const auto [ax, ay, ar] = TouchControls::layout(TouchControls::a, aspect);
        const auto [bx, by, br] = TouchControls::layout(TouchControls::b, aspect);
        const auto [yx, yy, yr] = TouchControls::layout(TouchControls::y, aspect);
        const float a_to_b = std::hypot((bx - ax) * aspect, by - ay), a_to_y = std::hypot((yx - ax) * aspect, yy - ay);
        require(std::fabs(a_to_b * a_to_b * 2.0f - a_to_y * a_to_y) < 1e-4f, "the diamond keeps its shape");
        for (size_t i = TouchControls::a; i < sfr::TouchOverlay::circles; ++i)
            for (size_t j = i + 1; j < sfr::TouchOverlay::circles; ++j) {
                const auto [x0, y0, r0] = TouchControls::layout(TouchControls::Button(i), aspect);
                const auto [x1, y1, r1] = TouchControls::layout(TouchControls::Button(j), aspect);
                require(std::hypot((x1 - x0) * aspect, y1 - y0) > r0 + r1, "two controls overlap");
            }

        // Touches read round buttons: as far right as up, in heights.
        TouchControls controls;
        controls.set_aspect(aspect);
        const auto [cx, cy, radius] = TouchControls::layout(TouchControls::b, aspect);
        controls.update(std::vector<TouchPoint>{{1, cx + radius * 1.2f / aspect, cy}});
        require(controls.state().buttons == button::b, "a touch beside B, inside its slack, presses it");
        controls.update(std::vector<TouchPoint>{{1, cx, cy - radius * 1.2f}});
        require(controls.state().buttons == button::b, "a touch above B, as far, presses it");
        controls.update(std::vector<TouchPoint>{{1, cx + radius * 1.35f / aspect, cy}});
        require(!(controls.state().buttons & button::b), "a touch past B's slack misses it");
        controls.update({});
        // The overlay draws what the touches read.
        const auto drawn = controls.overlay().circle[TouchControls::b];
        require(drawn[0] == cx && drawn[1] == cy && drawn[2] == radius, "the overlay follows the aspect");
        require(sfr::touch_overlay_at_rest(aspect).circle[TouchControls::b][0] == cx, "at rest as well");
        const auto rest = sfr::touch_overlay_at_rest(aspect);
        require(rest.right_knob[0] == rest.circle[TouchControls::right_stick][0] &&
                    rest.right_knob[1] == rest.circle[TouchControls::right_stick][1],
                "the right knob rests on its base");
        // A stick pushed its radius to the right (in heights) is a full push,
        // and the knob stops at the rim, in heights too.
        const auto [rx, ry, rr] = TouchControls::layout(TouchControls::right_stick, aspect);
        controls.update(std::vector<TouchPoint>{{2, rx, ry}}, 0.0f, true);
        controls.update(std::vector<TouchPoint>{{2, rx + rr * 2.0f / aspect, ry}}, 0.0f, true);
        require(controls.state().thumb_rx == 32767 && controls.state().thumb_ry == 0, "the right stick reads round");
        require(std::fabs((controls.overlay().right_knob[0] - rx) * aspect - rr) < 1e-4f, "the right knob stops at the rim");
        controls.update(std::vector<TouchPoint>{{2, rx + rr * 0.5f / aspect, ry}}, 0.0f, true);
        require(controls.state().thumb_rx > 16000 && controls.state().thumb_rx < 16500, "half a radius is half a push");
        controls.update({}, 0.0f, true);
    }
}

// Issue #443: each control hidden (neither drawn nor read: its place is
// empty screen) or drawn without its name (a negative state for the blit).
void options() {
    using sfr::TouchScene;
    namespace names = sfr::touch_options;
    require(names::parse("ls,lt") == ((1u << TouchControls::stick) | (1u << TouchControls::lt)), "a list of names is a mask");
    require(names::parse(" LS , Start,rs\r") ==
                ((1u << TouchControls::stick) | (1u << TouchControls::start) | (1u << TouchControls::right_stick)),
            "names in any case, with spaces");
    require(names::parse("") == 0 && names::parse("dpad,,a") == (1u << TouchControls::a), "unknown names are ignored");
    require(names::format(0).empty() && names::format(names::all) == "ls,a,b,x,y,rt,start,lb,rb,lt,rs",
            "a mask as a list, in the circles' order");
    for (uint32_t mask : {0u, 1u, 0x2A5u, names::all}) require(names::parse(names::format(mask)) == mask, "a list reads back");
    const sfr::TouchOptions read = sfr::touch_options_from("ls,rt", nullptr);
    require(read.hidden == ((1u << TouchControls::stick) | (1u << TouchControls::trigger)) && read.unnamed == 0 &&
                sfr::touch_options_from(nullptr, nullptr).hidden == 0,
            "the variables read as masks; unset is none");

    TouchControls controls;
    controls.update({});
    for (const auto& circle : controls.overlay().circle) require(circle[3] == 1.0f, "by default every control is shown and named");

    // Without names: drawn as before, the state negative.
    controls.set_options({0, (1u << TouchControls::a) | (1u << TouchControls::stick)});
    controls.update(std::vector<TouchPoint>{at(1, TouchControls::a)});
    require(controls.state().buttons == button::a, "a control without its name still works");
    require(controls.overlay().circle[TouchControls::a][3] == -2.0f && controls.overlay().circle[TouchControls::stick][3] == -1.0f &&
                controls.overlay().circle[TouchControls::b][3] == 1.0f,
            "only the chosen controls lose their names");
    controls.update({});

    // A hidden button is not drawn and does not react: in the menus its place
    // is the pointer, in a race nothing, on the title START.
    controls.set_options({1u << TouchControls::a, 0});
    controls.update(std::vector<TouchPoint>{at(2, TouchControls::a)}, 0.0f, TouchScene::menu);
    require(!controls.state().buttons && controls.pointer(), "a hidden A's place is the pointer in the menus");
    require(controls.overlay().circle[TouchControls::a][3] == 0.0f, "a hidden button is not drawn");
    controls.update({}, 0.0f, TouchScene::menu);
    controls.update(std::vector<TouchPoint>{at(3, TouchControls::a)}, 0.0f, TouchScene::race);
    require(controls.state() == sfr::GamepadState{} && !controls.pointer(), "a hidden A's place does nothing in a race");
    controls.update({}, 0.0f, TouchScene::race);
    controls.update(std::vector<TouchPoint>{at(4, TouchControls::a)}, 0.0f, TouchScene::title);
    require(controls.state().buttons == button::start, "a hidden A's place is START on the title, as empty screen");
    controls.update({}, 0.0f, TouchScene::title);
    controls.update(std::vector<TouchPoint>{at(5, TouchControls::b)}, 0.0f, TouchScene::race);
    require(controls.state().buttons == button::b, "the other buttons still work");
    controls.update({}, 0.0f, TouchScene::race);

    // LB hidden: in a race its place is the left side's empty screen, where
    // the floating stick starts (Codex review); with LS hidden too, nothing.
    controls.set_options({1u << TouchControls::lb, 0});
    controls.update(std::vector<TouchPoint>{at(11, TouchControls::lb)}, 0.0f, TouchScene::race);
    controls.update(std::vector<TouchPoint>{at(11, TouchControls::lb, 0.05f)}, 0.0f, TouchScene::race);
    require(!(controls.state().buttons & button::left_shoulder) && controls.state().thumb_lx > 0,
            "a hidden LB's place in a race starts the stick, as the left side's empty screen does");
    controls.update({}, 0.0f, TouchScene::race);
    controls.set_options({(1u << TouchControls::lb) | (1u << TouchControls::stick), 0});
    controls.update(std::vector<TouchPoint>{at(12, TouchControls::lb)}, 0.0f, TouchScene::race);
    controls.update(std::vector<TouchPoint>{at(12, TouchControls::lb, 0.05f)}, 0.0f, TouchScene::race);
    require(controls.state() == sfr::GamepadState{}, "with LS hidden as well it is nothing");
    controls.update({}, 0.0f, TouchScene::race);

    // The stick hidden (a player who steers by tilting): the left of the
    // screen starts no stick, and tilt steers.
    controls.set_options({1u << TouchControls::stick, 0});
    controls.update(std::vector<TouchPoint>{{6, 0.2f, 0.6f}}, -0.5f, TouchScene::race);
    controls.update(std::vector<TouchPoint>{{6, 0.3f, 0.6f}}, -0.5f, TouchScene::race);
    require(controls.state().thumb_lx < -16000 && controls.state().thumb_lx > -16500 && !controls.state().buttons,
            "with the stick hidden a finger on the left is nothing, and tilt steers");
    const sfr::TouchOverlay hidden_stick = controls.overlay();
    require(hidden_stick.circle[TouchControls::stick][3] == 0.0f, "the hidden stick is not drawn, held or not");
    controls.update({}, 0.0f, TouchScene::race);
    controls.update(std::vector<TouchPoint>{at(7, TouchControls::stick)}, 0.0f, TouchScene::menu);
    controls.update(std::vector<TouchPoint>{at(7, TouchControls::stick, 0.1f)}, 0.0f, TouchScene::menu);
    require(!controls.state().buttons && controls.pointer(), "in the menus the hidden stick's circle is the pointer");
    controls.update({}, 0.0f, TouchScene::menu);

    // The right stick hidden: its circle is empty screen too.
    controls.set_options({1u << TouchControls::right_stick, 0});
    controls.update(std::vector<TouchPoint>{at(8, TouchControls::right_stick, 0.03f)}, 0.0f, TouchScene::race);
    require(controls.state().thumb_rx == 0 && controls.overlay().circle[TouchControls::right_stick][3] == 0.0f,
            "a hidden right stick neither moves nor shows");
    controls.update({}, 0.0f, TouchScene::race);

    // START hidden: in a story scene a touch is still START (nothing is drawn there anyway).
    controls.set_options({1u << TouchControls::start, 0});
    controls.update(std::vector<TouchPoint>{at(9, TouchControls::start)}, 0.0f, TouchScene::scene);
    require(controls.state().buttons == button::start, "a scene's touch is START whatever is hidden");
    controls.update({}, 0.0f, TouchScene::scene);
    controls.update(std::vector<TouchPoint>{at(10, TouchControls::start)}, 0.0f, TouchScene::race);
    require(!controls.state().buttons, "a hidden START does not pause a race");
    controls.update({}, 0.0f, TouchScene::race);

    // Everything hidden at a wide aspect (Issue #443's round layout): nothing drawn.
    controls.set_options({names::all, 0});
    controls.set_aspect(19.5f / 9.0f);
    controls.update({});
    for (const auto& circle : controls.overlay().circle) require(circle[3] == 0.0f, "every control hidden draws none");

    // The blit's halves keep the sign: -1 and -2 survive packing.
    sfr::TouchOverlay overlay;
    overlay.circle[TouchControls::a] = {0.5f, 0.5f, 0.1f, -2.0f};
    const auto words = sfr::pack_touch_overlay(overlay);
    require(sfr::half_to_float(uint16_t(words[3 + TouchControls::a * 2] >> 16)) == -2.0f, "a negative state reaches the blit");
}

// Issue #443's layout editor: the touches follow the moved and resized
// controls (touch_layout.h), in each shape of image.
void placed() {
    namespace layout = sfr::touch_layout;
    using sfr::TouchScene;
    const auto put = [](TouchControls::Button button, float x, float y, float scale, float aspect) {
        return layout::place_at(button, x, y, scale, aspect);
    };
    for (const float aspect : {16.0f / 9.0f, 19.5f / 9.0f, 20.0f / 9.0f}) {
        TouchControls controls;
        controls.set_aspect(aspect);
        const auto old_a = controls.place(TouchControls::a);
        // A moved to the middle of the screen, B twice as big, X smaller.
        sfr::TouchLayout custom;
        custom.places[TouchControls::a] = put(TouchControls::a, 0.5f, 0.5f, 1.0f, aspect);
        custom.places[TouchControls::b] = put(TouchControls::b, 0.9f, 0.6f, 2.0f, aspect);
        custom.places[TouchControls::x] = put(TouchControls::x, 0.75f, 0.6f, 0.7f, aspect);
        controls.set_layout(custom);
        const auto [ax, ay, ar] = controls.place(TouchControls::a);
        require(std::abs(ax - 0.5f) < 0.001f && std::abs(ay - 0.5f) < 0.001f && ar == 0.075f, "A is where it was put");
        controls.update(std::vector<TouchPoint>{{1, ax, ay}}, 0.0f, TouchScene::race);
        require(controls.state().buttons == button::a, "A presses where it now is");
        controls.update({}, 0.0f, TouchScene::race);
        controls.update(std::vector<TouchPoint>{{2, old_a[0], old_a[1]}}, 0.0f, TouchScene::race);
        require(!(controls.state().buttons & button::a), "and not where it was");
        controls.update({}, 0.0f, TouchScene::race);
        // The drawn circles are the placed ones.
        const auto overlay = controls.overlay();
        require(overlay.circle[TouchControls::a][0] == ax && overlay.circle[TouchControls::a][1] == ay &&
                    overlay.circle[TouchControls::b][2] == 0.15f,
                "the blit draws the placed circles");
        // Twice as big reacts twice as far out; smaller, less far.
        const auto [bx, by, br] = controls.place(TouchControls::b);
        controls.update(std::vector<TouchPoint>{{3, bx, by + 0.075f * 1.8f}}, 0.0f, TouchScene::race);
        require(controls.state().buttons == button::b, "a bigger B reacts further from its centre");
        controls.update({}, 0.0f, TouchScene::race);
        const auto [xx, xy, xr] = controls.place(TouchControls::x);
        controls.update(std::vector<TouchPoint>{{4, xx, xy - 0.075f * 1.1f}}, 0.0f, TouchScene::race);
        require(!(controls.state().buttons & button::x), "a smaller X does not reach as far as before");
        controls.update({}, 0.0f, TouchScene::race);
        controls.update(std::vector<TouchPoint>{{40, xx, xy - xr * 0.9f}}, 0.0f, TouchScene::race);
        require(controls.state().buttons == button::x, "but reacts on itself");
        controls.update({}, 0.0f, TouchScene::race);

        // Overlap: B over A, a little to the right. A touch inside both
        // presses the nearer centre's (in radii); only in both slacks, both.
        sfr::TouchLayout over;
        over.places[TouchControls::a] = put(TouchControls::a, 0.5f, 0.5f, 1.0f, aspect);
        over.places[TouchControls::b] = put(TouchControls::b, 0.5f + 0.06f / aspect, 0.5f, 1.0f, aspect);
        controls.set_layout(over);
        controls.update(std::vector<TouchPoint>{{5, 0.5f, 0.5f}}, 0.0f, TouchScene::race);
        require(controls.state().buttons == button::a, "on A's centre only A");
        controls.update(std::vector<TouchPoint>{{6, 0.5f + 0.06f / aspect, 0.5f}}, 0.0f, TouchScene::race);
        require(controls.state().buttons == button::b, "on B's centre only B");
        controls.update(std::vector<TouchPoint>{{7, 0.5f + 0.03f / aspect, 0.5f + 0.083f}}, 0.0f, TouchScene::race);
        require(controls.state().buttons == (button::a | button::b), "just outside both, both (as a thumb across two)");
        controls.update({}, 0.0f, TouchScene::race);
        controls.update({}, 0.0f, TouchScene::race);
        // A button put over the stick's side wins over the floating stick.
        sfr::TouchLayout on_stick;
        on_stick.places[TouchControls::y] = put(TouchControls::y, 0.2f, 0.3f, 1.0f, aspect);
        controls.set_layout(on_stick);
        const auto [yx, yy, yr] = controls.place(TouchControls::y);
        controls.update(std::vector<TouchPoint>{{8, yx, yy}}, 0.0f, TouchScene::race);
        require(controls.state().buttons == button::y && controls.state().thumb_lx == 0, "a button beats the stick's side");
        controls.update({}, 0.0f, TouchScene::race);

        // LS bigger and higher: its circle in the menus, its travel in a race.
        sfr::TouchLayout big_stick;
        big_stick.places[TouchControls::stick] = put(TouchControls::stick, 0.2f, 0.45f, 1.5f, aspect);
        controls.set_layout(big_stick);
        const auto [sx, sy, sr] = controls.place(TouchControls::stick);
        require(std::abs(sr - 0.195f) < 1e-5f, "LS one and a half times as big");
        controls.update(std::vector<TouchPoint>{{9, sx, sy - sr}}, 0.0f, TouchScene::menu);
        controls.update(std::vector<TouchPoint>{{9, sx, sy - sr - 0.15f}}, 0.0f, TouchScene::menu);
        require(controls.state().buttons == button::dpad_up, "the menus' stick starts on its new circle");
        controls.update({}, 0.0f, TouchScene::menu);
        const auto rest = controls.overlay();
        require(rest.circle[TouchControls::stick][0] == sx && rest.circle[TouchControls::stick][1] == sy &&
                    rest.knob[0] == sx && rest.knob[1] == sy,
                "the stick rests where it was put");
        controls.update(std::vector<TouchPoint>{{10, 0.2f, 0.6f}}, 0.0f, TouchScene::race);
        controls.update(std::vector<TouchPoint>{{10, 0.2f + 0.13f / aspect, 0.6f}}, 0.0f, TouchScene::race);
        const int lx = controls.state().thumb_lx;
        require(lx > 21000 && lx < 22700, "the bigger stick needs a longer push for full lean (2/3 here)");
        controls.update({}, 0.0f, TouchScene::race);

        // LS put on the right: the floating stick moves to the right 40%.
        sfr::TouchLayout right_stick;
        right_stick.places[TouchControls::stick] = put(TouchControls::stick, 0.8f, 0.3f, 1.0f, aspect);
        controls.set_layout(right_stick);
        controls.update(std::vector<TouchPoint>{{11, 0.2f, 0.6f}}, 0.0f, TouchScene::race);
        controls.update(std::vector<TouchPoint>{{11, 0.25f, 0.6f}}, 0.0f, TouchScene::race);
        require(controls.state() == sfr::GamepadState{}, "the left side is empty once LS is on the right");
        controls.update({}, 0.0f, TouchScene::race);
        controls.update(std::vector<TouchPoint>{{12, 0.75f, 0.2f}}, 0.0f, TouchScene::race);
        controls.update(std::vector<TouchPoint>{{12, 0.7f, 0.2f}}, 0.0f, TouchScene::race);
        require(controls.state().thumb_lx < -16000, "the stick starts on the right side");
        controls.update({}, 0.0f, TouchScene::race);
        // LS just past 40% on the left: the side reaches its circle.
        sfr::TouchLayout inner_stick;
        inner_stick.places[TouchControls::stick] = put(TouchControls::stick, 0.42f, 0.5f, 1.0f, aspect);
        controls.set_layout(inner_stick);
        const auto inner = controls.place(TouchControls::stick);
        controls.update(std::vector<TouchPoint>{{13, inner[0] + inner[2] / aspect, 0.5f}}, 0.0f, TouchScene::race);
        controls.update(std::vector<TouchPoint>{{13, inner[0], 0.5f}}, 0.0f, TouchScene::race);
        require(controls.state().thumb_lx < -16000, "the side reaches a stick put past it");
        controls.update({}, 0.0f, TouchScene::race);
        // The default layout: the left 40% as before.
        controls.set_layout({});
        controls.update(std::vector<TouchPoint>{{14, 0.39f, 0.2f}}, 0.0f, TouchScene::race);
        controls.update(std::vector<TouchPoint>{{14, 0.30f, 0.2f}}, 0.0f, TouchScene::race);
        require(controls.state().thumb_lx < -16000, "by default the stick's side is the left 40%");
        controls.update({}, 0.0f, TouchScene::race);
        controls.update(std::vector<TouchPoint>{{15, 0.41f, 0.2f}}, 0.0f, TouchScene::race);
        controls.update(std::vector<TouchPoint>{{15, 0.32f, 0.2f}}, 0.0f, TouchScene::race);
        require(controls.state() == sfr::GamepadState{}, "and no further");
        controls.update({}, 0.0f, TouchScene::race);

        // The right stick moved: its base and knob follow.
        sfr::TouchLayout moved_rs;
        moved_rs.places[TouchControls::right_stick] = put(TouchControls::right_stick, 0.55f, 0.25f, 1.0f, aspect);
        controls.set_layout(moved_rs);
        const auto [rx, ry, rr] = controls.place(TouchControls::right_stick);
        controls.update(std::vector<TouchPoint>{{16, rx, ry}}, 0.0f, TouchScene::race);
        controls.update(std::vector<TouchPoint>{{16, rx + rr / aspect, ry}}, 0.0f, TouchScene::race);
        require(controls.state().thumb_rx == 32767, "the right stick works where it was put");
        controls.update({}, 0.0f, TouchScene::race);
        require(controls.overlay().right_knob[0] == rx && controls.overlay().right_knob[1] == ry, "and rests there");
    }
    // The default layout presses, at every point, every button whose circle
    // or slack the touch is in, exactly as before the editor; also where
    // default circles meet (LT and RT at 5:4).
    for (const float aspect : {16.0f / 9.0f, 19.5f / 9.0f, 20.0f / 9.0f, 5.0f / 4.0f}) {
        TouchControls controls;
        controls.set_aspect(aspect);
        const std::pair<TouchControls::Button, uint16_t> buttons[] = {
            {TouchControls::a, button::a}, {TouchControls::b, button::b}, {TouchControls::x, button::x},
            {TouchControls::y, button::y}, {TouchControls::start, button::start},
            {TouchControls::lb, button::left_shoulder}, {TouchControls::rb, button::right_shoulder}};
        int both = 0;
        for (float gx = 0.0f; gx <= 1.0f; gx += 0.004f)
            for (float gy = 0.0f; gy <= 1.0f; gy += 0.004f) {
                uint16_t expect = 0;
                bool rt = false, lt = false;
                for (const auto& [control, bit] : buttons) {
                    const auto [cx, cy, cr] = TouchControls::layout(control, aspect);
                    if (std::hypot((gx - cx) * aspect, gy - cy) <= cr * 1.25f) expect |= bit;
                }
                for (const auto control : {TouchControls::trigger, TouchControls::lt}) {
                    const auto [cx, cy, cr] = TouchControls::layout(control, aspect);
                    if (std::hypot((gx - cx) * aspect, gy - cy) <= cr * 1.25f) (control == TouchControls::lt ? lt : rt) = true;
                }
                if (!expect && !rt && !lt) continue;
                controls.update(std::vector<TouchPoint>{{71, gx, gy}}, 0.0f, TouchScene::race);
                const auto state = controls.state();
                require(state.buttons == expect && (state.right_trigger == 255) == rt && (state.left_trigger == 255) == lt,
                        "the default layout presses what it always did");
                both += rt && lt;
                controls.update({}, 0.0f, TouchScene::race);
            }
        if (aspect < 1.5f) require(both > 0, "at 5:4 the default LT and RT meet, and press together");
    }
    // The game reads the launcher's list from SFR_TOUCH_LAYOUT (the at-rest
    // overlay a desktop shows uses it too).
    const auto read = sfr::touch_layout_chosen();
    require(read.places[TouchControls::a] && read.places[TouchControls::a]->anchor == 'c', "SFR_TOUCH_LAYOUT is read");
    const auto rest = sfr::touch_overlay_at_rest(16.0f / 9.0f);
    require(std::abs(rest.circle[TouchControls::a][0] - 0.5f) < 1e-4f && std::abs(rest.circle[TouchControls::a][1] - 0.3f) < 1e-4f,
            "the controls at rest follow it");
}

int main() {
#ifdef _WIN32
    _putenv_s("SFR_TOUCH_CONTROLS", "1");
#else
    setenv("SFR_TOUCH_CONTROLS", "1", 1);
#endif
#ifdef _WIN32
    _putenv_s("SFR_TOUCH_LAYOUT", "a:c0.000,0.300,1.00");
#else
    setenv("SFR_TOUCH_LAYOUT", "a:c0.000,0.300,1.00", 1);
#endif
    try {
        run();
        race_actions();
        pointer();
        scenes();
        packing();
        controller_handover();
        tilt_lean();
        aspects();
        options();
        placed();
        std::cout << "Touch control checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
