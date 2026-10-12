#include "pause_gesture.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdlib>

namespace sfr {

PauseGesture PauseGesture::from_environment() {
    const char* const off = std::getenv("SFR_PAUSE_GESTURE");
    double seconds = 3.0;
    if (const char* text = std::getenv("SFR_PAUSE_GESTURE_SECONDS"); text && *text) {
        char* end = nullptr;
        const double value = std::strtod(text, &end);
        if (end != text && value >= 0.5 && value <= 10.0) seconds = value;
    }
    return PauseGesture(!(off && *off == '0'), seconds);
}

bool PauseGesture::in_pose(const SkeletonJoints& joints, const std::array<uint32_t, nui_joint_count>& states) {
    // A Kinect's guess at a joint it cannot see is no pose: in the side
    // stance a race is played in, the far arm is only inferred.
    namespace joint = nui_joint;
    for (const uint32_t j : {joint::shoulder_left, joint::elbow_left, joint::hand_left, joint::shoulder_right,
                             joint::hand_right})
        if (states[j] != 2) return false;
    return in_pose(joints);
}

bool PauseGesture::in_pose(const SkeletonJoints& joints) {
    namespace joint = nui_joint;
    // Facing the sensor: the shoulders spread across, not one behind the
    // other. A rider stands side-on, and from the side a leading arm held
    // forward and down looks like the pose's arm out and down.
    {
        const auto& left = joints[joint::shoulder_left];
        const auto& right = joints[joint::shoulder_right];
        const float across = std::fabs(right[0] - left[0]);
        const float width = std::hypot(right[0] - left[0], right[1] - left[1], right[2] - left[2]);
        if (!std::isfinite(width) || across < 0.2f || across < 0.7f * width) return false;
    }
    const auto& shoulder = joints[joint::shoulder_left];
    const auto& elbow = joints[joint::elbow_left];
    const auto& hand = joints[joint::hand_left];
    const auto distance = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
    };
    const float reach = distance(shoulder, hand);
    const float arm = distance(shoulder, elbow) + distance(elbow, hand);
    if (!std::isfinite(reach) || !std::isfinite(arm) || arm < 0.2f) return false;
    if (reach < 0.85f * arm) return false;  // bent
    const float outward = shoulder[0] - hand[0];  // the player's left is -x
    const float down = shoulder[1] - hand[1];
    if (!(outward > 0) || !(down > 0)) return false;
    const float below = std::atan2(down, outward) * 180.0f / 3.14159265f;
    if (below < 25.0f || below > 65.0f) return false;
    if (std::fabs(hand[2] - shoulder[2]) > 0.5f * reach) return false;  // sideways, not at the sensor
    // The other arm down by the side: the right hand below its shoulder,
    // within 30 degrees of straight down.
    const auto& right_shoulder = joints[joint::shoulder_right];
    const auto& right_hand = joints[joint::hand_right];
    const float hang = right_shoulder[1] - right_hand[1];
    const float aside = std::hypot(right_hand[0] - right_shoulder[0], right_hand[2] - right_shoulder[2]);
    return hang > 0.15f && std::atan2(aside, hang) * 180.0f / 3.14159265f <= 30.0f;
}

void PauseGesture::forget() {
    charge_ = 0;
    armed_ = true;
    started_ = false;
}

float PauseGesture::palm_facing(const SkeletonJoints& joints, int side, const HandPoints& tips, const HandPoints& thumbs) {
    const auto& hand = joints[side == 0 ? nui_joint::hand_left : nui_joint::hand_right];
    const auto& tip = tips[side];
    const auto& thumb = thumbs[side];
    const std::array<float, 3> up{tip[0] - hand[0], tip[1] - hand[1], tip[2] - hand[2]};
    const std::array<float, 3> across{thumb[0] - hand[0], thumb[1] - hand[1], thumb[2] - hand[2]};
    // The palm's normal: fingers up and the thumb towards the body's middle,
    // a left palm facing the sensor gives up x across pointing at it (-z),
    // a right one across x up.
    const auto& a = side == 0 ? up : across;
    const auto& b = side == 0 ? across : up;
    const std::array<float, 3> normal{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    const float length = std::hypot(normal[0], normal[1], normal[2]);
    if (!(length > 1e-6f) || (tip == std::array<float, 3>{} && thumb == std::array<float, 3>{}))
        return std::numeric_limits<float>::quiet_NaN();
    return normal[2] / length;
}

bool PauseGesture::stop_sign(const SkeletonJoints& joints, const std::array<uint8_t, 2>& hand_states,
                             const HandPoints& tips, const HandPoints& thumbs) {
    namespace joint = nui_joint;
    const auto& middle = joints[joint::shoulder_center];
    const auto& head = joints[joint::head];
    const uint32_t hands[2] = {joint::hand_left, joint::hand_right};
    for (int side = 0; side < 2; ++side) {
        if (hand_states[side] != open_hand) continue;
        const auto& hand = joints[hands[side]];
        const auto& other = joints[hands[1 - side]];
        if (!std::isfinite(hand[0]) || !std::isfinite(hand[1]) || !std::isfinite(hand[2])) continue;
        const bool high = hand[1] >= middle[1] - 0.05f && hand[1] <= (std::max)(head[1], middle[1]) + 0.25f;
        const bool central = std::fabs(hand[0] - middle[0]) <= 0.35f;
        const bool forward = middle[2] - hand[2] >= 0.1f;
        const bool fingers_up = tips[side][1] > hand[1];
        const bool facing = palm_facing(joints, side, tips, thumbs) <= -0.5f;  // NaN is not
        const bool other_down = !(other[1] >= middle[1] - 0.05f);
        if (high && central && forward && fingers_up && facing && other_down) return true;
    }
    return false;
}

bool PauseGesture::update(bool posed, Clock::time_point now) {
    if (!enabled_) return false;
    double step = 0;
    if (started_) step = (std::min)(std::chrono::duration<double>(now - last_).count(), max_step_seconds);
    if (!(step >= 0)) step = 0;
    started_ = true;
    last_ = now;
    if (!armed_) {
        // Fired: nothing charges until the arm has come out of the pose.
        if (!posed) armed_ = true;
        return false;
    }
    // Out of the pose it drains four times as fast as it charges, so a rider
    // who keeps half-matching the pose in play does not save up a pause.
    charge_ += (posed ? step : -drain_rate * step) / hold_seconds_;
    if (charge_ <= 0) {
        charge_ = 0;
        return false;
    }
    if (charge_ < 1) return false;
    charge_ = 0;
    armed_ = false;
    return true;
}

}
