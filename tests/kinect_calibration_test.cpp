#include "kinect_calibration.h"
#include "kinect_sensor.h"
#include "nui_skeleton.h"

#include <cmath>
#include <cstdio>
#include <functional>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAILED: %s\n", what);
        ++failures;
    }
}

// A body whose hips are `height` above a level floor (y = 0) and whose trunk
// leans `degrees` to the side (positive x).
sfr::KinectBody body(float height, float degrees) {
    sfr::KinectBody b;
    b.joints[sfr::nui_joint::hip_center] = {0.0f, height, 2.5f};
    const float radians = degrees / 57.29578f;
    b.joints[sfr::nui_joint::shoulder_center] = {0.5f * std::sin(radians), height + 0.5f * std::cos(radians), 2.5f};
    b.joint_states[sfr::nui_joint::hip_center] = 2;
    b.joint_states[sfr::nui_joint::shoulder_center] = 2;
    return b;
}

constexpr std::array<float, 4> level_floor{0.0f, 1.0f, 0.0f, 0.0f};

// Feeds 30 frames a second for `seconds`, the body from `at(t)` (t 0..1).
double run(sfr::KinectCalibration& calibration, double now, double seconds,
           const std::function<sfr::KinectBody(double)>& at) {
    const int frames = int(seconds * 30);
    for (int i = 0; i < frames; ++i) {
        const sfr::KinectBody b = at(double(i) / frames);
        calibration.observe(&b, level_floor, now);
        now += 1.0 / 30.0;
    }
    return now;
}
}

int main() {
    using Step = sfr::KinectCalibrationStep;
    sfr::KinectCalibration c;
    check(c.step() == Step::idle, "starts idle");
    c.start();
    double t = 0;
    // Standing still with a 1 cm sway: the stance is taken after 1.5 s.
    t = run(c, t, 2.0, [](double p) { return body(0.90f + 0.005f * float(std::sin(p * 20)), 1.0f); });
    check(c.step() == Step::crouch, "standing still leads to the crouch");
    check(std::fabs(c.result().standing_height - 0.90f) < 0.01f, "standing height measured");
    // A 12 cm crouch, down and up again (Issue #97's player).
    t = run(c, t, 1.0, [](double p) { return body(0.90f - 0.12f * float(std::sin(p * 3.14159)), 1.0f); });
    t = run(c, t, 0.3, [](double) { return body(0.90f, 1.0f); });
    check(c.step() == Step::jump, "the crouch leads to the jump");
    check(std::fabs(c.result().crouch_drop - 0.12f) < 0.01f, "crouch depth measured");
    check(c.result().crouch_cm == 8, "suggested depth is 70% of the crouch");
    // A 10 cm jump.
    t = run(c, t, 0.6, [](double p) { return body(0.90f + 0.10f * float(std::sin(p * 3.14159)), 1.0f); });
    t = run(c, t, 0.3, [](double) { return body(0.90f, 1.0f); });
    check(c.step() == Step::lean_left, "the jump leads to the leans");
    check(std::fabs(c.result().jump_rise - 0.10f) < 0.01f, "jump height measured");
    // Leaning 15 degrees one way (from the 1 degree stance), then upright.
    t = run(c, t, 1.0, [](double p) { return body(0.90f, 1.0f - 15.0f * float(std::sin(p * 3.14159))); });
    t = run(c, t, 0.3, [](double) { return body(0.90f, 1.0f); });
    check(c.step() == Step::lean_right, "the first lean leads to the second");
    check(std::fabs(c.result().lean_left - 15.0f) < 0.5f, "first lean measured");
    // Leaning the same way again does not count as the other side.
    t = run(c, t, 1.0, [](double p) { return body(0.90f, 1.0f - 15.0f * float(std::sin(p * 3.14159))); });
    t = run(c, t, 0.3, [](double) { return body(0.90f, 1.0f); });
    check(c.step() == Step::lean_right, "the same side again is not the other lean");
    // Then 18 degrees the other way.
    t = run(c, t, 1.0, [](double p) { return body(0.90f, 1.0f + 18.0f * float(std::sin(p * 3.14159))); });
    t = run(c, t, 0.3, [](double) { return body(0.90f, 1.0f); });
    check(c.step() == Step::done, "both leans finish it");
    check(std::fabs(c.result().lean_right - 18.0f) < 0.5f, "second lean measured");

    // Nobody tracked: standing still never completes.
    c.start();
    for (int i = 0; i < 90; ++i) c.observe(nullptr, level_floor, i / 30.0);
    check(c.step() == Step::stand && !c.tracked(), "nobody tracked stays on standing");
    // Swaying more than the band keeps restarting the stance.
    run(c, 3.0, 3.0, [](double p) { return body(0.90f + 0.05f * float(std::sin(p * 40)), 0.0f); });
    check(c.step() == Step::stand, "a body that does not stand still is not taken");

    // A shallow crouch still gives the smallest depth; a deep one the largest.
    check(sfr::KinectCalibration::crouch_cm_for(0.04f) == 5, "depth at least 5 cm");
    check(sfr::KinectCalibration::crouch_cm_for(0.80f) == 35, "depth at most 35 cm");
    // No floor from the sensor: the hips' own height still gives the drop.
    c.start();
    const std::array<float, 4> no_floor{};
    double u = 0;
    for (int i = 0; i < 60; ++i) { const auto b = body(0.30f, 0.0f); c.observe(&b, no_floor, u); u += 1 / 30.0; }
    for (int i = 0; i < 30; ++i) {
        const auto b = body(0.30f - 0.20f * float(std::sin(i / 30.0 * 3.14159)), 0.0f);
        c.observe(&b, no_floor, u);
        u += 1 / 30.0;
    }
    for (int i = 0; i < 10; ++i) { const auto b = body(0.30f, 0.0f); c.observe(&b, no_floor, u); u += 1 / 30.0; }
    check(c.result().crouch_cm == 14, "without a floor the hips' height gives the depth");

    if (!failures) std::printf("kinect calibration checks passed\n");
    return failures ? 1 : 0;
}
