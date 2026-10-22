#pragma once
#include "kinect_sensor.h"
#include "nui_transform_smooth.h"

#include <array>
#include <cstdint>
#include <unordered_map>

namespace sfr {

// The NUI library's NuiTransformSmooth (nui_transform_smooth.h: Holt's
// double exponential smoothing with a jitter radius and a cap on how far a
// prediction may run, step for step as the title's library has it) over a
// host frame's bodies, keyed by tracking id. Only for SFR_KINECT_SMOOTH=1
// (kinect_host_smoothing: sfr_kinect_probe --smooth, tests): the game
// smooths a real sensor's skeletons itself, once, in its own
// NuiTransformSmooth (nui_smooth_hooks.cpp).
using KinectSmoothingParameters = NuiSmoothParameters;

class KinectJointSmoother {
public:
    explicit KinectJointSmoother(KinectSmoothingParameters parameters = {}) : parameters_(parameters) {}
    // Smooths every body in the frame in place, as the library smooths a
    // TRACKED skeleton (inferred joints get twice the radii; a joint at the
    // origin starts over). A body seen for the first time starts from its
    // own measurement; bodies that have left the frame are forgotten.
    void apply(KinectFrame& frame);

private:
    KinectSmoothingParameters parameters_;
    std::unordered_map<uint32_t, NuiSmoothSkeleton> bodies_;
};

}
