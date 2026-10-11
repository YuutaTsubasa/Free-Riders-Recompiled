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
    bool update(const SkeletonJoints& joints, Clock::time_point now) { return update(in_pose(joints), now); }
    // The same, told whether a pose is held (the Kinect v2's open palm too).
    bool update(bool posed, Clock::time_point now);
    void forget();
    // How full the gauge is, 0 to 1 (0 once it has fired, until the arm has
    // come down).
    float progress() const { return float(charge_); }

    static bool in_pose(const SkeletonJoints& joints);

    // The Kinect v2's own pause, the Stop Sign: a hand held up in front of
    // the face, open, fingers up, palm towards the sensor, either hand, the
    // other one down. Only a v2 can see it: it reads the hand as open (hand
    // state 2) and tracks the fingertip and the thumb, from which the palm's
    // facing comes (wrist-side hand point, fingertip, thumb). The hand is
    // between the shoulders' height and a little above the head, within
    // 0.35 m of the body's middle sideways and at least 0.1 m in front of
    // the shoulders; the palm faces within 60 degrees of the sensor; the
    // other hand is below the shoulders, so both arms up in a race is not
    // it. It charges the same gauge.
    static constexpr uint8_t open_hand = 2;
    using HandPoints = std::array<std::array<float, 3>, 2>;  // left, right
    static bool stop_sign(const SkeletonJoints& joints, const std::array<uint8_t, 2>& hand_states,
                          const HandPoints& tips, const HandPoints& thumbs);
    // Which way a palm faces along the sensor's axis, -1 straight at it to
    // 1 straight away, or NaN without its fingertip and thumb (for the log).
    static float palm_facing(const SkeletonJoints& joints, int side, const HandPoints& tips, const HandPoints& thumbs);

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
