#include "camera_tuner.h"

#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
using sfr::CameraStep;
using sfr::CameraStepState;

// A player in a 640x480 picture, shoulders at y 120, ankles at y 430.
sfr::PoseLandmarks player(float shift_x = 0, float shift_y = 0) {
    using namespace sfr::pose_point;
    sfr::PoseLandmarks body{};
    const auto set = [&](uint32_t p, float x, float y) { body[p] = {x + shift_x, y + shift_y, 0.9f}; };
    set(shoulder_left, 360, 120); set(shoulder_right, 280, 120);
    set(hip_left, 345, 250); set(hip_right, 295, 250);
    set(knee_left, 345, 340); set(knee_right, 295, 340);
    set(ankle_left, 345, 430); set(ankle_right, 295, 430);
    return body;
}
}

int main() {
    try {
        sfr::CameraReadinessInput input;
        auto r = sfr::camera_readiness(input);
        require(r.first_failed == CameraStep::camera && !r.ready(), "no camera fails at the camera");

        input.opened = true;
        input.seconds_open = 0.5;
        r = sfr::camera_readiness(input);
        require(r.first_failed == CameraStep::count && r.steps[size_t(CameraStep::pictures)] == CameraStepState::waiting,
                "a window just opened waits for the first picture");
        input.seconds_open = 4;
        require(sfr::camera_readiness(input).first_failed == CameraStep::pictures, "no picture in three seconds fails");
        input.seconds_since_picture = 2;
        require(sfr::camera_readiness(input).first_failed == CameraStep::pictures, "pictures that stopped fail");

        input.seconds_since_picture = 0.03;
        require(sfr::camera_readiness(input).first_failed == CameraStep::body, "pictures with nobody fail at the body");
        input.found = true;
        input.motion = false;
        require(sfr::camera_readiness(input).first_failed == CameraStep::body, "no pose model finds no body");
        input.motion = true;
        r = sfr::camera_readiness(input);
        require(r.first_failed == CameraStep::whole_body, "a body cut off fails at the whole body");
        input.whole_body = true;
        r = sfr::camera_readiness(input);
        require(r.ready() && r.first_failed == CameraStep::count, "the whole body in the picture passes every step");

        auto framing = sfr::camera_framing(player(), 640, 480);
        require(framing.whole_body && framing.advice == sfr::CameraAdvice::none, "a body in the middle is in view");
        framing = sfr::camera_framing(player(0, 40), 640, 480);
        require(!framing.whole_body && framing.advice == sfr::CameraAdvice::step_back, "ankles off the bottom: step back");
        auto unseen = player();
        unseen[sfr::pose_point::ankle_left].score = 0.2f;
        require(sfr::camera_framing(unseen, 640, 480).advice == sfr::CameraAdvice::step_back, "an ankle not seen: step back");
        framing = sfr::camera_framing(player(280, 0), 640, 480);
        require(!framing.whole_body && framing.advice == sfr::CameraAdvice::to_the_middle, "off a side: to the middle");
    } catch (const std::exception& error) {
        std::cerr << "camera_tuner_test: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
