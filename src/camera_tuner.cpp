#include "camera_tuner.h"

namespace sfr {

CameraReadiness camera_readiness(const CameraReadinessInput& input) {
    CameraReadiness r;
    auto& s = r.steps;
    const auto fail = [&](CameraStep step) {
        s[size_t(step)] = CameraStepState::failed;
        if (r.first_failed == CameraStep::count) r.first_failed = step;
    };
    if (!input.opened) {
        fail(CameraStep::camera);
        return r;
    }
    s[size_t(CameraStep::camera)] = CameraStepState::passed;
    const bool arriving = input.seconds_since_picture >= 0 && input.seconds_since_picture < camera_picture_timeout_seconds;
    if (!arriving) {
        // A window just opened waits for the first picture; after that, or
        // when pictures stopped, the step has failed.
        if (input.seconds_since_picture >= 0 || input.seconds_open >= camera_first_picture_seconds) fail(CameraStep::pictures);
        return r;
    }
    s[size_t(CameraStep::pictures)] = CameraStepState::passed;
    if (!input.motion || !input.found) {
        fail(CameraStep::body);
        return r;
    }
    s[size_t(CameraStep::body)] = CameraStepState::passed;
    if (!input.whole_body) fail(CameraStep::whole_body);
    else s[size_t(CameraStep::whole_body)] = CameraStepState::passed;
    return r;
}

CameraFraming camera_framing(const PoseLandmarks& body, uint32_t width, uint32_t height) {
    using namespace pose_point;
    CameraFraming framing;
    if (!width || !height) return framing;
    const float margin_x = float(width) / 20, margin_y = float(height) / 20;
    bool off_vertical = false, off_side = false;
    for (uint32_t point : {shoulder_left, shoulder_right, hip_left, hip_right, knee_left, knee_right, ankle_left, ankle_right}) {
        const PoseLandmark& p = body[point];
        if (!(p.score >= 0.5f) || p.y < margin_y || p.y > float(height) - margin_y) off_vertical = true;
        else if (p.x < margin_x || p.x > float(width) - margin_x) off_side = true;
    }
    framing.advice = off_vertical ? CameraAdvice::step_back : off_side ? CameraAdvice::to_the_middle : CameraAdvice::none;
    framing.whole_body = framing.advice == CameraAdvice::none;
    return framing;
}

}
