#pragma once
#include <array>
#include <cstdint>

namespace sfr {
struct KinectBody;

// The Kinect calibration a player walks through in the Kinect preview, like
// the console's own: stand where the sensor sees you, stand still, crouch as
// you would in a race, jump, lean each way. It measures the two things the
// race reads that differ from body to body - how far the hips drop in a
// crouch (the launcher's crouch depth, SFR_KINECT_CROUCH_DEPTH: Issues #97
// and #132, where a 12 cm crouch never reached the 15 cm default) and how far
// the player leans - and suggests the crouch depth to set.
//
// Heights are of the hip centre above the floor the sensor reports, or the
// hips' own height when it reports none (only differences are used).
enum class KinectCalibrationStep : uint8_t { idle, stand, crouch, jump, lean_left, lean_right, done };

struct KinectCalibrationResult {
    float standing_height = 0;  // metres above the floor
    float crouch_drop = 0;      // metres, the deepest of the crouch
    float jump_rise = 0;        // metres above standing, 0 when no jump was seen
    float lean_left = 0, lean_right = 0;  // degrees from upright, each way
    uint32_t crouch_cm = 0;     // the crouch depth to set
};

class KinectCalibration {
public:
    // Begins again from the first step.
    void start();
    void cancel() { step_ = KinectCalibrationStep::idle; }
    // One sensor frame: the player's body (null when nobody is tracked), the
    // floor plane (ax + by + cz + d, all zero when unknown) and the time.
    void observe(const KinectBody* body, const std::array<float, 4>& floor, double seconds);

    KinectCalibrationStep step() const { return step_; }
    // How far the current step is, 0..1 (standing still, holding a lean).
    float progress() const { return progress_; }
    // Live values for the window: the hips' drop below standing (negative
    // above it), and the lean from upright in degrees (left negative).
    float drop() const { return drop_; }
    float lean() const { return lean_; }
    bool tracked() const { return tracked_; }
    // The step timed out waiting for the motion (a crouch or a lean it did
    // not see); the step stays, so the player can try again.
    bool waiting_long() const { return waiting_long_; }
    const KinectCalibrationResult& result() const { return result_; }

    // A crouch counts when the hips drop this much below standing; the
    // crouch depth suggested is this share of the player's own crouch, so a
    // race's crouch, a little shallower than the calibration's, still counts.
    static constexpr float crouch_share = 0.7f;
    static uint32_t crouch_cm_for(float drop_metres);

private:
    void advance(KinectCalibrationStep next, double seconds);
    KinectCalibrationStep step_ = KinectCalibrationStep::idle;
    KinectCalibrationResult result_;
    double step_started_ = 0, still_since_ = -1;
    float still_low_ = 0, still_high_ = 0, still_sum_ = 0, still_lean_sum_ = 0;
    int still_count_ = 0;
    float neutral_lean_ = 0;
    float progress_ = 0, drop_ = 0, lean_ = 0;
    float deepest_ = 0, highest_ = 0, leaned_ = 0;
    float leaned_sign_ = 0;  // which way the first lean went
    bool tracked_ = false, waiting_long_ = false, moved_ = false;
};
}
