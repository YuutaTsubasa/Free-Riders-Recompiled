#include "pause_gesture.h"

#include <cmath>
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

bool PauseGesture::in_pose(const SkeletonJoints& joints) {
    namespace joint = nui_joint;
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

bool PauseGesture::update(const SkeletonJoints& joints, Clock::time_point now) {
    if (!enabled_) return false;
    double step = 0;
    if (started_) step = (std::min)(std::chrono::duration<double>(now - last_).count(), max_step_seconds);
    if (!(step >= 0)) step = 0;
    started_ = true;
    last_ = now;
    const bool posed = in_pose(joints);
    if (!armed_) {
        // Fired: nothing charges until the arm has come out of the pose.
        if (!posed) armed_ = true;
        return false;
    }
    charge_ += (posed ? step : -step) / hold_seconds_;
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
