#include "kinect_smoothing.h"

#include <cmath>
#include <unordered_set>

namespace sfr {
namespace {
using Vec = std::array<float, 3>;
Vec add(const Vec& a, const Vec& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec sub(const Vec& a, const Vec& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec scale(const Vec& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
Vec mix(const Vec& a, const Vec& b, float t) { return add(scale(a, 1.0f - t), scale(b, t)); }  // a towards b
float length(const Vec& a) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }
}

void KinectJointSmoother::apply(KinectFrame& frame) {
    std::unordered_set<uint32_t> seen;
    for (auto& body : frame.bodies) {
        seen.insert(body.tracking_id);
        Body& history = bodies_[body.tracking_id];
        for (uint32_t j = 0; j < nui_joint_count; ++j) {
            Joint& joint = history[j];
            const uint32_t state = body.joint_states[j];
            if (state == 0) {  // not tracked: as measured, and start over
                joint = {};
                continue;
            }
            // Inferred joints are guesses; the SDK smooths them twice as hard.
            const float jitter = parameters_.jitter_radius * (state == 1 ? 2.0f : 1.0f);
            const float deviation = parameters_.max_deviation * (state == 1 ? 2.0f : 1.0f);
            const Vec measured = body.joints[j];
            Vec raw = measured, filtered, trend;
            if (joint.frames == 0) {
                filtered = raw;
                trend = {};
            } else if (joint.frames == 1) {
                filtered = scale(add(raw, joint.raw), 0.5f);
                trend = mix(joint.trend, sub(filtered, joint.filtered), parameters_.correction);
            } else {
                // Inside the jitter radius, pull the measurement towards the
                // last output in proportion to how small the move is.
                const float moved = length(sub(raw, joint.filtered));
                if (moved <= jitter && jitter > 0) raw = mix(joint.filtered, raw, moved / jitter);
                filtered = mix(raw, add(joint.filtered, joint.trend), parameters_.smoothing);
                trend = mix(joint.trend, sub(filtered, joint.filtered), parameters_.correction);
            }
            Vec predicted = add(filtered, scale(trend, parameters_.prediction));
            // Never lead the measurement by more than the deviation radius.
            const float lead = length(sub(predicted, measured));
            if (lead > deviation && lead > 0) predicted = mix(measured, predicted, deviation / lead);
            joint.raw = measured;
            joint.filtered = filtered;
            joint.trend = trend;
            joint.frames = joint.frames < 2 ? joint.frames + 1 : 2;
            body.joints[j] = predicted;
        }
    }
    for (auto it = bodies_.begin(); it != bodies_.end();)
        it = seen.count(it->first) ? std::next(it) : bodies_.erase(it);
}

}
