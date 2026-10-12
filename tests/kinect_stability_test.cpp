#include "kinect_stability.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool near(float a, float b, float within) { return std::fabs(a - b) <= within; }

namespace joint = sfr::nui_joint;

// A player standing still 2.4 m from the sensor, arms down (sensor space:
// metres, +y up), every joint tracked.
sfr::KinectBody standing(uint32_t id = 7) {
    sfr::KinectBody body;
    body.tracking_id = id;
    const auto set = [&](uint32_t j, float x, float y) {
        body.joints[j] = {x, y, 2.4f};
        body.joint_states[j] = 2;
    };
    set(joint::hip_center, 0, 0);
    set(joint::spine, 0, 0.2f);
    set(joint::shoulder_center, 0, 0.5f);
    set(joint::head, 0, 0.7f);
    set(joint::shoulder_left, -0.18f, 0.5f);
    set(joint::elbow_left, -0.2f, 0.22f);
    set(joint::wrist_left, -0.21f, -0.03f);
    set(joint::hand_left, -0.21f, -0.1f);
    set(joint::shoulder_right, 0.18f, 0.5f);
    set(joint::elbow_right, 0.2f, 0.22f);
    set(joint::wrist_right, 0.21f, -0.03f);
    set(joint::hand_right, 0.21f, -0.1f);
    set(joint::hip_left, -0.1f, 0);
    set(joint::knee_left, -0.1f, -0.42f);
    set(joint::ankle_left, -0.1f, -0.82f);
    set(joint::foot_left, -0.1f, -0.88f);
    set(joint::hip_right, 0.1f, 0);
    set(joint::knee_right, 0.1f, -0.42f);
    set(joint::ankle_right, 0.1f, -0.82f);
    set(joint::foot_right, 0.1f, -0.88f);
    return body;
}

// The right hand raised as a cursor, wandering a few millimetres.
std::vector<sfr::KinectBody> pointing(int step, uint32_t id = 7, float x = 0.3f) {
    std::vector<sfr::KinectBody> bodies{standing(id)};
    auto& hand = bodies[0].joints[joint::hand_right];
    hand = {x + 0.004f * std::sin(float(step) * 2.7f), 0.6f + 0.003f * std::cos(float(step) * 1.9f), 2.1f};
    bodies[0].joints[joint::wrist_right] = {hand[0], hand[1] - 0.05f, 2.15f};
    return bodies;
}

void the_menu_cursor_settles() {
    sfr::KinectBodySteadying steadying;
    float worst = 0;
    for (int step = 0; step < 60; ++step) {
        auto bodies = pointing(step);
        steadying.steady(bodies, true);
        if (step) worst = std::fmax(worst, std::fabs(bodies[0].joints[joint::hand_right][0] - 0.3f));
    }
    require(worst <= 0.0045f, "a hand held on a button holds the cursor");
}

void racing_hands_pass_untouched() {
    sfr::KinectBodySteadying steadying;
    for (int step = 0; step < 20; ++step) {
        auto bodies = pointing(step);
        const auto given = bodies[0].joints;
        steadying.steady(bodies, false);
        require(bodies[0].joints == given, "while racing the skeleton is the sensor's");
    }
}

void each_body_keeps_its_own_memory() {
    sfr::KinectBodySteadying steadying;
    for (int step = 0; step < 10; ++step) {
        auto bodies = pointing(step);
        steadying.steady(bodies, true);
    }
    // A second player pointing elsewhere is not pulled to the first one's cursor.
    auto first = pointing(10);
    auto second = pointing(10, 9, -0.1f);
    std::vector<sfr::KinectBody> bodies{first[0], second[0]};
    const float given = bodies[1].joints[joint::hand_right][0];
    steadying.steady(bodies, true);
    require(near(bodies[1].joints[joint::hand_right][0], given, 1e-6f), "a new body starts from where its own hand is");
}

// A hand the sensor stops tracking is left as given and starts over, so its
// return is not steadied towards a stale rest point.
void an_untracked_hand_starts_over() {
    sfr::KinectBodySteadying steadying;
    for (int step = 0; step < 10; ++step) {
        auto bodies = pointing(step);
        steadying.steady(bodies, true);
    }
    auto lost = pointing(10);
    lost[0].joints[joint::hand_right] = {0, 0, 0};
    lost[0].joint_states[joint::hand_right] = 0;
    steadying.steady(lost, true);
    require(lost[0].joints[joint::hand_right] == (std::array<float, 3>{0, 0, 0}), "an untracked hand passes through");
    auto back = pointing(11, 7, -0.05f);
    const float given = back[0].joints[joint::hand_right][0];
    steadying.steady(back, true);
    require(near(back[0].joints[joint::hand_right][0], given, 1e-6f), "back in view it starts from where it is");
}

// A tracking id gone for a second is someone else when it comes back.
void a_reused_id_is_someone_new() {
    sfr::KinectBodySteadying steadying;
    for (int step = 0; step < 10; ++step) {
        auto bodies = pointing(step);
        steadying.steady(bodies, true);
    }
    for (int step = 0; step < 31; ++step) {
        std::vector<sfr::KinectBody> nobody;
        steadying.steady(nobody, true);
    }
    auto again = pointing(0, 7, -0.1f);
    const float given = again[0].joints[joint::hand_right][0];
    steadying.steady(again, true);
    require(near(again[0].joints[joint::hand_right][0], given, 1e-6f), "the id's old rest point is forgotten");
}

void it_can_be_turned_off() {
    sfr::KinectBodySteadying steadying(false);
    for (int step = 0; step < 10; ++step) {
        auto bodies = pointing(step);
        const auto given = bodies[0].joints;
        steadying.steady(bodies, true);
        require(bodies[0].joints == given, "SFR_MENU_HAND_STEADY=0 leaves the hands alone");
    }
}
}

int main() {
    try {
        the_menu_cursor_settles();
        racing_hands_pass_untouched();
        each_body_keeps_its_own_memory();
        an_untracked_hand_starts_over();
        a_reused_id_is_someone_new();
        it_can_be_turned_off();
    } catch (const std::exception& error) {
        std::cerr << "kinect_stability_test: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
