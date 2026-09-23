#include "pose_smoothing.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

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
}

int main() {
    try {
        a_still_hand_stops_wandering();
        a_moving_hand_is_not_held_back();
        a_gap_starts_again();
        nothing_is_smoothed_when_it_is_turned_off();
        std::cout << "Pose smoothing checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
