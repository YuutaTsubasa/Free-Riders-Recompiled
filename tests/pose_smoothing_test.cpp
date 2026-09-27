#include "pose_smoothing.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <limits>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

constexpr double interval = 1.0 / 30.0;  // a camera's thirty pictures a second

sfr::PoseLandmarks at(float x, float y) {
    sfr::PoseLandmarks landmarks{};
    for (auto& point : landmarks) point = {x, y, 0.9f};
    return landmarks;
}

// The wandering of a point the model reads afresh from every picture.
float noise(int step) { return std::sin(float(step) * 2.7f) * 2.0f; }

void a_still_hand_stops_wandering() {
    sfr::PoseSmoothing smoothing(1.0f, 0.05f);
    float worst_in = 0, worst_out = 0;
    for (int step = 0; step < 90; ++step) {
        auto landmarks = at(320.0f + noise(step), 240.0f);
        const float given = landmarks[sfr::pose_point::wrist_right].x;
        smoothing.smooth(landmarks, interval);
        if (step < 30) continue;  // the filter settles first
        worst_in = (std::max)(worst_in, std::fabs(given - 320.0f));
        worst_out = (std::max)(worst_out, std::fabs(landmarks[sfr::pose_point::wrist_right].x - 320.0f));
    }
    require(worst_in > 1.5f, "the test's own noise is worth filtering");
    require(worst_out < worst_in * 0.35f, "a still point wanders far less afterwards");
}

void a_moving_hand_is_not_held_back() {
    sfr::PoseSmoothing smoothing(1.0f, 0.05f);
    float x = 320;
    for (int step = 0; step < 30; ++step) {  // half a second at 300 pixels a second
        auto landmarks = at(x, 240.0f);
        smoothing.smooth(landmarks, interval);
        if (step == 29)
            require(std::fabs(landmarks[sfr::pose_point::wrist_right].x - x) < 25.0f,
                    "the smoothed point keeps up with a real movement");
        x += 300.0f * float(interval);
    }
}

void a_gap_starts_again() {
    sfr::PoseSmoothing smoothing(1.0f, 0.05f);
    for (int step = 0; step < 30; ++step) {
        auto landmarks = at(100.0f, 100.0f);
        smoothing.smooth(landmarks, interval);
    }
    auto landmarks = at(500.0f, 400.0f);
    smoothing.smooth(landmarks, 3.0);  // the camera stalled, or the player left
    require(landmarks[sfr::pose_point::wrist_right].x == 500.0f, "after a gap the new place is taken as it is");
}

void nothing_is_smoothed_when_it_is_turned_off() {
    sfr::PoseSmoothing smoothing(0.0f, 0.05f);
    auto landmarks = at(1.0f, 2.0f);
    smoothing.smooth(landmarks, interval);
    smoothing.smooth(landmarks, interval);
    require(landmarks[sfr::pose_point::nose].x == 1.0f, "a cutoff of zero passes the points through");
}

sfr::PoseLandmarks world_at(float value) {
    auto landmarks = at(320, 240);
    for (auto& point : landmarks) {
        point.world = {value, value, value};
        point.has_world = true;
    }
    return landmarks;
}

void world_noise_is_smoothed_in_all_axes() {
    sfr::PoseSmoothing smoothing(1.0f, 0.05f);
    float worst = 0;
    for (int step = 0; step < 90; ++step) {
        auto landmarks = world_at(noise(step) / 250.0f);
        smoothing.smooth(landmarks, interval);
        if (step >= 30)
            for (const float axis : landmarks[sfr::pose_point::wrist_right].world)
                worst = (std::max)(worst, std::fabs(axis));
        require(landmarks[sfr::pose_point::wrist_right].has_world, "smoothing retains valid world points");
    }
    require(worst < 0.0028f, "world XYZ noise is damped to less than 35 percent");
}

void world_motion_uses_metre_appropriate_velocity() {
    sfr::PoseSmoothing smoothing(1.0f, 0.05f);
    for (int step = 0; step < 30; ++step) {
        const float value = float(step) * 1.2f * float(interval);
        auto landmarks = world_at(value);
        smoothing.smooth(landmarks, interval);
        if (step == 29)
            for (const float axis : landmarks[sfr::pose_point::wrist_right].world) {
                require(axis < value, "world movement is actually filtered");
                require(value - axis < 0.1f, "world filter keeps up with a reaching hand");
            }
        require(landmarks[sfr::pose_point::wrist_right].x == 320, "world motion cannot leak into image axes");
    }
}

void world_state_resets_after_gaps_or_missing_points() {
    for (int reset = 0; reset < 5; ++reset) {
        sfr::PoseSmoothing smoothing(1.0f, 0.05f);
        auto landmarks = world_at(0);
        smoothing.smooth(landmarks, interval);
        if (reset == 0) smoothing.forget();
        if (reset == 1) { auto missing = at(320, 240); smoothing.smooth(missing, interval); }
        if (reset == 2) { auto lost = world_at(0); for (auto& point : lost) point.score = 0; smoothing.smooth(lost, interval); }
        landmarks = world_at(0.8f);
        smoothing.smooth(landmarks, reset == 3 ? 2.0 : reset == 4 ? std::numeric_limits<double>::quiet_NaN() : interval);
        for (const float axis : landmarks[sfr::pose_point::wrist_right].world)
            require(axis == 0.8f, "reacquisition starts world XYZ at the current measurement");
    }
}

void nonfinite_measurements_do_not_poison_future_frames() {
    sfr::PoseSmoothing smoothing(1.0f, 0.05f);
    auto landmarks = world_at(0);
    smoothing.smooth(landmarks, interval);
    landmarks[sfr::pose_point::wrist_right].world[2] = std::numeric_limits<float>::quiet_NaN();
    landmarks[sfr::pose_point::wrist_right].x = std::numeric_limits<float>::infinity();
    smoothing.smooth(landmarks, interval);
    require(!landmarks[sfr::pose_point::wrist_right].has_world, "invalid world measurement is marked unavailable");
    landmarks = world_at(0.6f);
    smoothing.smooth(landmarks, interval);
    require(landmarks[sfr::pose_point::wrist_right].x == 320, "image state recovers after invalid input");
    for (const float axis : landmarks[sfr::pose_point::wrist_right].world)
        require(axis == 0.6f, "invalid world point resets all three axes");
}
}

int main() {
    try {
        a_still_hand_stops_wandering();
        a_moving_hand_is_not_held_back();
        a_gap_starts_again();
        nothing_is_smoothed_when_it_is_turned_off();
        world_noise_is_smoothed_in_all_axes();
        world_motion_uses_metre_appropriate_velocity();
        world_state_resets_after_gaps_or_missing_points();
        nonfinite_measurements_do_not_poison_future_frames();
        std::cout << "Pose smoothing checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
