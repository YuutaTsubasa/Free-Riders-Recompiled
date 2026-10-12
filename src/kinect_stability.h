#pragma once
#include "kinect_sensor.h"
#include "pose_stability.h"

#include <cstdint>
#include <vector>

namespace sfr {

// The webcam's menu steadying (pose_stability.h's MenuHandSteadying) on the
// Kinect's skeletons: in the menus the hands are a cursor, and a hand held
// still on a button still wanders a few millimetres between frames, so each
// tracked body's hands go through it while a menu is up. Each body keeps its
// own memory by tracking id; a body that leaves takes it with it.
// SFR_MENU_HAND_STEADY=0 turns it off, for the webcam too (menu_steadying
// in pose_stability.h).
//
// Holding hidden limb points and checking them against the depth image were
// tried here as well and taken out: on a real sensor they made no visible
// difference.
class KinectBodySteadying {
public:
    static KinectBodySteadying from_environment();
    explicit KinectBodySteadying(bool steady_menu = true) : steady_menu_(steady_menu) {}

    // A new frame of bodies, and whether a menu is up.
    void steady(std::vector<KinectBody>& bodies, bool in_menu);

private:
    struct Body {
        uint32_t tracking_id = 0;
        MenuHandSteadying hands;
        uint64_t last_seen = 0;
    };
    Body& body_for(uint32_t tracking_id);
    bool steady_menu_;
    std::vector<Body> bodies_;
    uint64_t frame_ = 0;
};

}
