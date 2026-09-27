#include "pose_smoothing.h"

#include <cmath>
#include <cstdlib>
#include <numbers>

namespace sfr {
namespace {
// The weight a low-pass filter gives the newest value, for a cutoff in Hz
// over an interval in seconds.
float smoothing_factor(float cutoff, double interval) {
    const double corner = 2.0 * std::numbers::pi * double(cutoff) * interval;
    return float(corner / (corner + 1.0));
}

float read_number(const char* name, float fallback) {
    const char* const text = std::getenv(name);
    if (!text || !*text) return fallback;
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    return end != text && value >= 0 ? float(value) : fallback;
}

// The speed a point is moving is itself measured between two noisy pictures,
// so it is low-passed at a fixed cutoff before it is allowed to open up the
// filter. One Hz is what the paper uses and what the noise here wants.
constexpr float speed_cutoff = 1.0f;
}

PoseSmoothing::PoseSmoothing(float cutoff_at_rest, float speed_coefficient)
    : cutoff_at_rest_(cutoff_at_rest), speed_coefficient_(speed_coefficient) {}

PoseSmoothing PoseSmoothing::from_environment() {
    // A second of hand at 200 pixels a second opens the cutoff to about 11 Hz,
    // which follows a real movement; a hand held still sits near 1 Hz.
    return PoseSmoothing(read_number("SFR_POSE_SMOOTHING", 1.0f), read_number("SFR_POSE_SMOOTHING_BETA", 0.05f));
}

void PoseSmoothing::forget() {
    for (auto& point : points_)
        for (auto& axis : point) axis.started = false;
    for (auto& point : world_points_)
        for (auto& axis : point) axis.started = false;
}

void PoseSmoothing::smooth_axis(Axis& axis, float& value, double interval, float speed_coefficient) {
    if (!std::isfinite(value)) {
        axis.started = false;
        return;
    }
    if (!axis.started) {
        axis = {value, 0.0f, true};
        return;
    }
    const float speed = float((double(value) - double(axis.value)) / interval);
    if (!std::isfinite(speed)) {
        axis = {value, 0.0f, true};
        return;
    }
    const float speed_weight = smoothing_factor(speed_cutoff, interval);
    axis.speed += speed_weight * (speed - axis.speed);
    const float cutoff = cutoff_at_rest_ + speed_coefficient * std::fabs(axis.speed);
    const float weight = smoothing_factor(cutoff, interval);
    axis.value += weight * (value - axis.value);
    value = axis.value;
}

void PoseSmoothing::smooth(PoseLandmarks& landmarks, double interval_seconds) {
    if (cutoff_at_rest_ <= 0) return;
    // A gap means the player was away, or the camera stalled: what was
    // remembered says nothing about where they are now.
    if (!std::isfinite(interval_seconds) || interval_seconds <= 0 || interval_seconds > 0.5) {
        forget();
        if (!std::isfinite(interval_seconds) || interval_seconds <= 0) return;
    }
    for (uint32_t point = 0; point < pose_point::count; ++point) {
        float* const values[2] = {&landmarks[point].x, &landmarks[point].y};
        for (uint32_t which = 0; which < 2; ++which) {
            smooth_axis(points_[point][which], *values[which], interval_seconds, speed_coefficient_);
        }
        auto& landmark = landmarks[point];
        bool valid_world = landmark.has_world && std::isfinite(landmark.score) && landmark.score > 0;
        for (const float value : landmark.world)
            valid_world = valid_world && std::isfinite(value) && std::fabs(value) <= 10.0f;
        if (!valid_world) {
            for (auto& axis : world_points_[point]) axis.started = false;
            landmark.has_world = false;
            continue;
        }
        for (uint32_t axis = 0; axis < 3; ++axis)
            smooth_axis(world_points_[point][axis], landmark.world[axis], interval_seconds, speed_coefficient_ * 250.0f);
    }
}

}
