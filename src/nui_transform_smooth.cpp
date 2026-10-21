#include "nui_transform_smooth.h"

#include <cmath>

namespace sfr {
namespace {
using Vec = std::array<float, 3>;
Vec add(const Vec& a, const Vec& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec sub(const Vec& a, const Vec& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec scale(const Vec& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
// a * s + b, as the library's vmaddfp.
Vec madd(const Vec& a, float s, const Vec& b) { return {a[0] * s + b[0], a[1] * s + b[1], a[2] * s + b[2]}; }
float length(const Vec& a) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }

// The library's smallest jitter radius (820C12D8).
constexpr float minimum_jitter_radius = 0.0001f;
constexpr uint32_t joint_inferred = 1;
}

Vec nui_smooth_joint(NuiSmoothJoint& history, const Vec& measured, uint32_t state, const NuiSmoothParameters& p) {
    float jitter = p.jitter_radius < minimum_jitter_radius ? minimum_jitter_radius : p.jitter_radius;
    float deviation = p.max_deviation_radius;
    if (state == joint_inferred) {
        jitter *= 2.0f;
        deviation *= 2.0f;
    }
    // A joint at the origin has no position: its history starts again.
    if (measured[0] == 0.0f && measured[1] == 0.0f && measured[2] == 0.0f) history.frames = 0;
    Vec filtered, trend;
    if (history.frames == 0) {
        filtered = measured;
        trend = {};
        history.frames = 1;
    } else if (history.frames == 1) {
        filtered = scale(add(measured, history.raw), 0.5f);
        trend = madd(sub(filtered, history.filtered), p.correction, scale(history.trend, 1.0f - p.correction));
        history.frames = 2;
    } else {
        Vec pulled = measured;
        const float moved = length(sub(measured, history.filtered));
        if (!(moved > jitter)) {
            const float t = moved / jitter;
            pulled = madd(measured, t, scale(history.filtered, 1.0f - t));
        }
        filtered = madd(pulled, 1.0f - p.smoothing, scale(add(history.filtered, history.trend), p.smoothing));
        trend = madd(sub(filtered, history.filtered), p.correction, scale(history.trend, 1.0f - p.correction));
    }
    Vec predicted = madd(trend, p.prediction, filtered);
    const float lead = length(sub(predicted, measured));
    if (lead > deviation) {
        const float t = deviation / lead;
        predicted = madd(predicted, t, scale(measured, 1.0f - t));
    }
    history.raw = measured;
    history.filtered = filtered;
    history.trend = trend;
    return predicted;
}

void nui_smooth_skeleton(NuiSmoothSkeleton& history, SkeletonJoints& joints,
                         const std::array<uint32_t, nui_joint_count>& states, const NuiSmoothParameters& parameters) {
    for (uint32_t j = 0; j < nui_joint_count; ++j)
        joints[j] = nui_smooth_joint(history[j], joints[j], states[j], parameters);
}

void NuiFrameSmoother::smooth(std::array<Slot, 6>& slots, const NuiSmoothParameters& parameters) {
    for (size_t s = 0; s < slots.size(); ++s) {
        Slot& slot = slots[s];
        if (slot.tracking_state != nui_tracked || slot.tracking_id != ids_[s]) {
            history_[s] = {};
            ids_[s] = slot.tracking_state == nui_tracked ? slot.tracking_id : 0;
        }
        if (slot.tracking_state != nui_tracked) continue;
        nui_smooth_skeleton(history_[s], slot.joints, slot.states, parameters);
    }
}

void NuiFrameSmoother::reset() {
    history_ = {};
    ids_ = {};
    has_last = false;
    last_frame = 0;
    last = {};
}

}
