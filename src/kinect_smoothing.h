#pragma once
#include "kinect_sensor.h"

#include <array>
#include <cstdint>
#include <unordered_map>

namespace sfr {

// Kinect v1's SDK smooths a skeleton frame with NuiTransformSmooth; the v2
// SDK has nothing of the kind, so its joints reach the game as measured and
// shake. This is the same filter -- Holt's double exponential smoothing with
// a jitter radius and a cap on how far a prediction may run -- applied to
// each tracked body's joints, keyed by its tracking id.
struct KinectSmoothingParameters {
    float smoothing = 0.5f;      // 0 follows the measurement, towards 1 the history
    float correction = 0.5f;     // how fast the trend follows the motion
    float prediction = 0.5f;     // frames of trend added ahead
    float jitter_radius = 0.05f; // metres: closer moves are damped
    float max_deviation = 0.04f; // metres: how far the output may lead the measurement
};

class KinectJointSmoother {
public:
    explicit KinectJointSmoother(KinectSmoothingParameters parameters = {}) : parameters_(parameters) {}
    // Smooths every body in the frame in place. A joint that is not tracked
    // passes through and starts over when it is seen again; inferred joints
    // get twice the jitter and deviation radii, as in the SDK. Bodies that
    // have left the frame are forgotten.
    void apply(KinectFrame& frame);

private:
    struct Joint {
        std::array<float, 3> raw{}, filtered{}, trend{};
        uint32_t frames = 0;
    };
    using Body = std::array<Joint, nui_joint_count>;
    KinectSmoothingParameters parameters_;
    std::unordered_map<uint32_t, Body> bodies_;
};

}
