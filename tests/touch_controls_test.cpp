#include "touch_controls.h"
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
}

int main() {
    try {
        run();
        std::cout << "Touch control checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
