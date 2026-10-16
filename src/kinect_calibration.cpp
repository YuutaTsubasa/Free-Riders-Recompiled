#include "kinect_calibration.h"
#include "kinect_sensor.h"
#include "nui_skeleton.h"

#include <algorithm>
#include <cmath>

namespace sfr {
namespace {
// Standing still: the hips within this band for this long.
constexpr float still_band = 0.03f;
constexpr double still_seconds = 1.5;
// A crouch or jump: the hips this far from standing, then back within the
// return band of it.
constexpr float crouch_least = 0.05f, jump_least = 0.05f, return_band = 0.025f;
// A lean: at least this far from upright, then back within the return lean.
constexpr float lean_least = 10.0f, lean_return = 5.0f;
// After this long without the motion, the window says to try it again.
constexpr double motion_wait = 10.0;
}

uint32_t KinectCalibration::crouch_cm_for(float drop_metres) {
    const long cm = std::lround(double(drop_metres) * 100.0 * crouch_share);
    return uint32_t(std::clamp(cm, 5L, 35L));
}

void KinectCalibration::start() {
    *this = KinectCalibration{};
    step_ = KinectCalibrationStep::stand;
    step_started_ = -1;  // set by the first frame
}

void KinectCalibration::advance(KinectCalibrationStep next, double seconds) {
    step_ = next;
    step_started_ = seconds;
    progress_ = 0;
    deepest_ = highest_ = leaned_ = 0;
    moved_ = waiting_long_ = false;
}

void KinectCalibration::observe(const KinectBody* body, const std::array<float, 4>& floor, double seconds) {
    if (step_ == KinectCalibrationStep::idle || step_ == KinectCalibrationStep::done) return;
    if (step_started_ < 0) step_started_ = seconds;
    tracked_ = body && body->joint_states[nui_joint::hip_center] && body->joint_states[nui_joint::shoulder_center];
    if (!tracked_) {
        still_since_ = -1;
        if (step_ == KinectCalibrationStep::stand) progress_ = 0;
        return;
    }
    const auto& hip = body->joints[nui_joint::hip_center];
    const auto& shoulders = body->joints[nui_joint::shoulder_center];
    const bool has_floor = floor[0] != 0.0f || floor[1] != 0.0f || floor[2] != 0.0f;
    const float height = has_floor ? floor[0] * hip[0] + floor[1] * hip[1] + floor[2] * hip[2] + floor[3] : hip[1];
    // The trunk's tilt to the side, from the hips up to the shoulders.
    const float tilt = std::atan2(shoulders[0] - hip[0], shoulders[1] - hip[1]) * 57.29578f;
    waiting_long_ = seconds - step_started_ > motion_wait;

    switch (step_) {
    case KinectCalibrationStep::stand: {
        // Standing still: every sample since still_since_ within the band.
        // One outside it starts the stillness again from itself.
        const float low = std::min(still_low_, height), high = std::max(still_high_, height);
        if (still_since_ < 0 || high - low > still_band) {
            still_since_ = seconds;
            still_low_ = still_high_ = height;
            still_sum_ = still_lean_sum_ = 0;
            still_count_ = 0;
        } else {
            still_low_ = low;
            still_high_ = high;
        }
        still_sum_ += height;
        still_lean_sum_ += tilt;
        ++still_count_;
        progress_ = float(std::min(1.0, (seconds - still_since_) / still_seconds));
        drop_ = 0;
        lean_ = 0;
        if (seconds - still_since_ >= still_seconds && still_count_ > 0) {
            result_.standing_height = still_sum_ / float(still_count_);
            neutral_lean_ = still_lean_sum_ / float(still_count_);
            advance(KinectCalibrationStep::crouch, seconds);
        }
        return;
    }
    default: break;
    }
    drop_ = result_.standing_height - height;
    lean_ = tilt - neutral_lean_;
    switch (step_) {
    case KinectCalibrationStep::crouch:
        deepest_ = std::max(deepest_, drop_);
        moved_ = moved_ || deepest_ >= crouch_least;
        progress_ = std::min(1.0f, deepest_ / 0.15f);
        if (moved_ && drop_ < return_band) {
            result_.crouch_drop = deepest_;
            result_.crouch_cm = crouch_cm_for(deepest_);
            advance(KinectCalibrationStep::jump, seconds);
        }
        break;
    case KinectCalibrationStep::jump:
        highest_ = std::max(highest_, -drop_);
        moved_ = moved_ || highest_ >= jump_least;
        progress_ = std::min(1.0f, highest_ / 0.10f);
        if (moved_ && -drop_ < return_band) {
            result_.jump_rise = highest_;
            advance(KinectCalibrationStep::lean_left, seconds);
        } else if (!moved_ && waiting_long_) {
            advance(KinectCalibrationStep::lean_left, seconds);  // no jump seen: the result says so
        }
        break;
    case KinectCalibrationStep::lean_left:
    case KinectCalibrationStep::lean_right: {
        // The two leans go opposite ways; which sign is left depends on the
        // sensor, so the first lean decides it.
        const bool second = step_ == KinectCalibrationStep::lean_right;
        const float toward = second ? -leaned_sign_ : 0.0f;
        const float amount = toward == 0.0f ? std::fabs(lean_) : std::max(0.0f, lean_ * toward);
        if (amount > std::fabs(leaned_)) leaned_ = toward == 0.0f ? lean_ : amount * toward;
        moved_ = moved_ || std::fabs(leaned_) >= lean_least;
        progress_ = std::min(1.0f, std::fabs(leaned_) / 30.0f);
        if (moved_ && std::fabs(lean_) < lean_return) {
            if (!second) {
                result_.lean_left = std::fabs(leaned_);
                leaned_sign_ = leaned_ < 0 ? -1.0f : 1.0f;
                advance(KinectCalibrationStep::lean_right, seconds);
            } else {
                result_.lean_right = std::fabs(leaned_);
                advance(KinectCalibrationStep::done, seconds);
            }
        }
        break;
    }
    default: break;
    }
}
}
