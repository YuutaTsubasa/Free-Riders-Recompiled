#pragma once
#include "pose_skeleton.h"

#include <chrono>
#include <cstdint>

namespace sfr {

// The Pause Gesture of the Xbox 360's Kinect (the "universal pause"): the
// right arm down by the side and the left arm held straight out and down,
// about 45 degrees from the body. On the console it is the system's, not the
// title's: holding it showed a gauge in the lower left and opened the game's
// pause menu. Here it pauses a race the way the voice command does (the
// title's "pauseopen"), from a Kinect's skeleton or the webcam's.
//
// Skeleton space as the title has it: metres, +y up, and the player's right
// is +x, so the left hand reaching out goes towards -x. The pose holds when
//   - the left arm is nearly straight: shoulder to hand is at least 85% of
//     the upper arm and forearm together;
//   - the left hand is out to the side and below the shoulder, 25 to 65
//     degrees under the horizontal;
//   - it points sideways rather than at the sensor: the hand is no more
//     than half the arm's length in front of or behind the shoulder;
//   - the right hand hangs by the side: below the right shoulder, within 30
//     degrees of straight down.
// The gauge charges while the pose holds and drains while it does not, at
// the same rate: hold_seconds of pose fill it from empty, and a moment out
// of the pose (a picture that lost the arm, a wobble) only takes back what
// it lasted instead of starting the wait again. Full, it fires once; it
// fires again only after the arm has left the pose.
class PauseGesture {
public:
    using Clock = std::chrono::steady_clock;
    // SFR_PAUSE_GESTURE=0 turns it off; SFR_PAUSE_GESTURE_SECONDS sets how
    // long to hold (default 3).
    static PauseGesture from_environment();
    explicit PauseGesture(bool enabled = true, double hold_seconds = 3.0)
        : enabled_(enabled), hold_seconds_(hold_seconds) {}

    // True once, at the moment the gauge fills.
    bool update(const SkeletonJoints& joints, Clock::time_point now);
    void forget();
    // How full the gauge is, 0 to 1 (0 once it has fired, until the arm has
    // come down).
    float progress() const { return float(charge_); }

    static bool in_pose(const SkeletonJoints& joints);

    // A gap between updates longer than this (a stall, a load) counts as
    // this long, so it cannot fill or empty the gauge at once.
    static constexpr double max_step_seconds = 0.1;

private:
    bool enabled_;
    double hold_seconds_;
    double charge_ = 0;
    bool armed_ = true, started_ = false;
    Clock::time_point last_{};
};

}
