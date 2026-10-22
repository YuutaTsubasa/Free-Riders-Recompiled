#include "kinect_smoothing.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

sfr::KinectFrame frame_with(float x, uint32_t state = 2, uint32_t id = 7) {
    sfr::KinectFrame frame;
    auto& body = frame.bodies.emplace_back();
    body.tracking_id = id;
    for (uint32_t j = 0; j < sfr::nui_joint_count; ++j) {
        body.joints[j] = {x, 1.0f, 2.5f};
        body.joint_states[j] = state;
    }
    return frame;
}

float smoothed_x(sfr::KinectJointSmoother& smoother, float x, uint32_t state = 2, uint32_t id = 7) {
    auto frame = frame_with(x, state, id);
    smoother.apply(frame);
    return frame.bodies[0].joints[3][0];
}

void a_still_joint_stays_put() {
    sfr::KinectJointSmoother smoother;
    for (int i = 0; i < 30; ++i)
        require(std::abs(smoothed_x(smoother, 0.2f) - 0.2f) < 1e-6f, "a joint that does not move is left where it is");
}

void noise_is_damped() {
    sfr::KinectJointSmoother smoother;
    float input = 0, output = 0;
    for (int i = 0; i < 120; ++i) {
        const float noise = (i % 2 ? 0.01f : -0.01f);  // the +-1 cm shake of a v2 joint
        const float out = smoothed_x(smoother, noise);
        if (i >= 20) { input += std::abs(noise); output += std::abs(out); }
    }
    require(output < input * 0.5f, "a centimetre of shake is more than halved");
}

void a_move_is_followed_without_running_ahead() {
    sfr::KinectJointSmoother smoother;
    for (int i = 0; i < 10; ++i) smoothed_x(smoother, 0.0f);
    float last = 0;
    for (int i = 0; i < 30; ++i) {
        const float measured = 0.5f;  // a lean of half a metre
        last = smoothed_x(smoother, measured);
        require(std::abs(last - measured) <= 0.04f + 1e-5f, "the output never strays past the deviation radius");
    }
    require(std::abs(last - 0.5f) < 0.01f, "after a second the joint has caught up");
    // Inferred joints may lead by twice as much, no more.
    sfr::KinectJointSmoother inferred;
    for (int i = 0; i < 10; ++i) smoothed_x(inferred, 0.0f, 1);
    for (int i = 0; i < 30; ++i)
        require(std::abs(smoothed_x(inferred, 0.5f, 1) - 0.5f) <= 0.08f + 1e-5f, "an inferred joint has twice the radius");
}

void lost_joints_and_new_bodies_start_over() {
    // As the NUI library does: a joint at the origin (no position) passes
    // as it is and starts its history over; a joint called not tracked that
    // still has a position is filtered like any other.
    sfr::KinectJointSmoother smoother;
    for (int i = 0; i < 10; ++i) smoothed_x(smoother, 0.2f);
    auto lost = frame_with(0.0f);
    for (auto& joint : lost.bodies[0].joints) joint = {0.0f, 0.0f, 0.0f};
    smoother.apply(lost);
    require(lost.bodies[0].joints[3] == (std::array<float, 3>{0.0f, 0.0f, 0.0f}), "a joint without a position passes");
    sfr::KinectJointSmoother untracked;
    smoothed_x(untracked, 0.0f, 0);
    require(std::abs(smoothed_x(untracked, 0.1f, 0) - 0.0625f) < 1e-6f, "a joint not tracked but placed is filtered");
    for (int i = 0; i < 10; ++i) smoothed_x(smoother, 0.0f, 2, 8);
    require(smoothed_x(smoother, 0.3f, 2, 9) == 0.3f, "a new body starts from its own measurement");
}

void the_same_filter_as_the_games() {
    // The backends' filter (SFR_KINECT_SMOOTH=1) is the game's own
    // NuiTransformSmooth step for step.
    sfr::KinectJointSmoother host;
    sfr::NuiFrameSmoother game;
    for (int i = 0; i < 60; ++i) {
        const float x = 0.3f * std::sin(float(i) * 0.3f) + (i % 3 ? 0.004f : -0.006f);
        auto frame = frame_with(x, i % 9 == 0 ? 1u : 2u);
        std::array<sfr::NuiFrameSmoother::Slot, 6> slots{};
        slots[2].tracking_state = 2;
        slots[2].tracking_id = 7;
        slots[2].joints = frame.bodies[0].joints;
        slots[2].states = frame.bodies[0].joint_states;
        host.apply(frame);
        game.smooth(slots, {});
        require(frame.bodies[0].joints == slots[2].joints, "the host's smoothing is the game's");
    }
}
}

int main() {
    try {
        a_still_joint_stays_put();
        noise_is_damped();
        a_move_is_followed_without_running_ahead();
        lost_joints_and_new_bodies_start_over();
        the_same_filter_as_the_games();
        std::cout << "Kinect smoothing checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
