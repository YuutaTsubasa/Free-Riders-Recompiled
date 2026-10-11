#include "pose_stability.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool near(float a, float b, float within) { return std::fabs(a - b) <= within; }

constexpr double interval = 1.0 / 30.0;  // a camera's thirty pictures a second
using namespace sfr::pose_point;

// A world-model body standing still, arms down, hips at the origin (metres;
// +y down, as the model gives them). Picture points follow at 200 px a metre.
sfr::PoseLandmarks standing(float score = 0.9f) {
    sfr::PoseLandmarks body{};
    const auto set = [&](uint32_t point, float x, float y, float z = 0) {
        body[point].world = {x, y, z};
        body[point].has_world = true;
        body[point].x = 320 + x * 200;
        body[point].y = 240 + y * 200;
        body[point].score = score;
    };
    set(nose, 0, -0.65f);
    for (uint32_t point : {eye_left, eye_right, ear_left, ear_right}) set(point, 0, -0.65f);
    set(shoulder_left, 0.18f, -0.5f);
    set(shoulder_right, -0.18f, -0.5f);
    set(elbow_left, 0.2f, -0.22f);
    set(elbow_right, -0.2f, -0.22f);
    set(wrist_left, 0.21f, 0.03f);
    set(wrist_right, -0.21f, 0.03f);
    set(hip_left, 0.1f, 0);
    set(hip_right, -0.1f, 0);
    set(knee_left, 0.1f, 0.42f);
    set(knee_right, -0.1f, 0.42f);
    set(ankle_left, 0.1f, 0.82f);
    set(ankle_right, -0.1f, 0.82f);
    return body;
}

void settle(sfr::PoseStabilizer& stabilizer, int pictures = 10) {
    for (int i = 0; i < pictures; ++i) {
        auto body = standing();
        stabilizer.stabilize(body, interval);
    }
}

void a_body_it_believes_passes_untouched() {
    sfr::PoseStabilizer stabilizer;
    for (int i = 0; i < 20; ++i) {
        auto body = standing();
        const auto given = body;
        stabilizer.stabilize(body, interval);
        require(stabilizer.held() == 0, "nothing is held of a believable body");
        for (uint32_t p = 0; p < count; ++p)
            require(body[p].x == given[p].x && body[p].world == given[p].world, "a believable body is left as it is");
    }
}

void a_hand_the_model_cannot_see_is_held_on_the_arm() {
    sfr::PoseStabilizer stabilizer;
    settle(stabilizer);
    // The hand passes behind the body: the model guesses it on the other side.
    auto body = standing();
    body[wrist_right].score = 0.2f;
    body[wrist_right].world = {0.3f, 0.0f, 0.0f};
    body[wrist_right].x = 320 + 0.3f * 200;
    stabilizer.stabilize(body, interval);
    require(stabilizer.held() == 1, "the unseen hand is held");
    require(near(body[wrist_right].world[0], -0.21f, 1e-4f) && near(body[wrist_right].x, 320 - 0.21f * 200, 1e-2f),
            "it stays where it was, in the world and in the picture");

    // The body steps aside while the hand is still unseen: the hand goes with it.
    auto moved = standing();
    for (auto& point : moved) { point.world[0] += 0.1f; point.x += 20; }
    moved[wrist_right].score = 0.2f;
    moved[wrist_right].world = {0.5f, 0.0f, 0.0f};
    stabilizer.stabilize(moved, interval);
    require(near(moved[wrist_right].world[0], -0.11f, 1e-4f), "a held hand moves with the arm it hangs from");
}

void a_hold_ends_and_the_model_is_believed_again() {
    sfr::PoseStabilizer stabilizer;
    settle(stabilizer);
    int held = 0;
    for (int i = 0; i < 20; ++i) {
        auto body = standing();
        body[wrist_left].score = 0.2f;
        body[wrist_left].world = {0.21f, -0.6f, 0.0f};
        stabilizer.stabilize(body, interval);
        if (stabilizer.held()) ++held;
        else require(near(body[wrist_left].world[1], -0.6f, 1e-4f), "after the hold the model's point is used");
    }
    require(held >= 6 && held <= 8, "a point is held for about a quarter of a second, once, no longer");
    // Believed again, then lost again: a new hold.
    auto seen = standing();
    stabilizer.stabilize(seen, interval);
    auto lost = standing();
    lost[wrist_left].score = 0.2f;
    lost[wrist_left].world = {0.21f, -0.6f, 0.0f};
    stabilizer.stabilize(lost, interval);
    require(stabilizer.held() == 1, "a point seen again can be held again");
}

void a_bone_that_grows_is_a_wrong_point() {
    sfr::PoseStabilizer stabilizer;
    settle(stabilizer);
    // The model is sure, but the forearm is twice its length.
    auto body = standing();
    body[wrist_right].world = {-0.2f, 0.28f, 0.0f};
    stabilizer.stabilize(body, interval);
    require(stabilizer.held() == 1 && near(body[wrist_right].world[1], 0.03f, 1e-4f),
            "a forearm twice as long as it has been is not believed");
}

void a_jump_is_ignored_but_a_movement_is_followed() {
    sfr::PoseStabilizer stabilizer;
    settle(stabilizer);
    // One picture with the hand thrown up by its full length, still the right
    // bone length: a jump.
    auto body = standing();
    body[wrist_right].world = {-0.2f, -0.47f, 0.0f};
    stabilizer.stabilize(body, interval);
    require(stabilizer.held() == 1 && near(body[wrist_right].world[1], 0.03f, 1e-4f), "a single jump is held");
    // It stays there: a real movement, believed by the third picture.
    bool followed = false;
    for (int i = 0; i < 3 && !followed; ++i) {
        auto up = standing();
        up[wrist_right].world = {-0.2f, -0.47f, 0.0f};
        stabilizer.stabilize(up, interval);
        followed = near(up[wrist_right].world[1], -0.47f, 1e-4f);
    }
    require(followed, "a jump that stays is a movement and is followed");
}

void a_fast_but_possible_movement_is_not_held() {
    sfr::PoseStabilizer stabilizer;
    settle(stabilizer);
    // A punch: 4 m/s, 13 cm a picture, the forearm turning about the elbow.
    for (int i = 1; i <= 3; ++i) {
        auto body = standing();
        const float angle = 0.5f * float(i);
        body[wrist_right].world = {-0.2f, -0.22f + 0.25f * std::cos(angle), -0.25f * std::sin(angle)};
        const auto given = body[wrist_right].world;
        stabilizer.stabilize(body, interval);
        require(stabilizer.held() == 0 && body[wrist_right].world == given, "a fast real movement passes untouched");
    }
}

void it_can_be_turned_off() {
    sfr::PoseStabilizer stabilizer(false);
    settle(stabilizer);
    auto body = standing();
    body[wrist_right].score = 0.1f;
    body[wrist_right].world = {0.4f, 0, 0};
    stabilizer.stabilize(body, interval);
    require(stabilizer.held() == 0 && body[wrist_right].world[0] == 0.4f, "SFR_POSE_STABILIZE=0 leaves every point alone");
}

sfr::SkeletonJoints skeleton(float hand_x, float hand_y) {
    sfr::SkeletonJoints joints{};
    for (auto& joint : joints) joint = {0, 0, 2.5f};
    joints[sfr::nui_joint::shoulder_center] = {0, 0.5f, 2.5f};
    joints[sfr::nui_joint::hand_right] = {hand_x, hand_y, 2.2f};
    joints[sfr::nui_joint::wrist_right] = {hand_x, hand_y - 0.02f, 2.2f};
    joints[sfr::nui_joint::hand_left] = {-0.2f, 0.0f, 2.5f};
    joints[sfr::nui_joint::wrist_left] = {-0.2f, 0.02f, 2.5f};
    return joints;
}

void a_still_hand_holds_the_menu_cursor() {
    sfr::MenuHandSteadying steadying;
    float worst = 0;
    for (int step = 0; step < 60; ++step) {
        // A few millimetres of wandering.
        auto joints = skeleton(0.3f + 0.004f * std::sin(float(step) * 2.7f), 0.5f + 0.003f * std::cos(float(step) * 1.9f));
        steadying.steady(joints, true);
        if (step) worst = std::fmax(worst, std::fabs(joints[sfr::nui_joint::hand_right][0] - 0.3f));
    }
    require(worst <= 0.0045f, "the cursor stays put");
    auto joints = skeleton(0.3f, 0.5f);
    steadying.steady(joints, true);
    require(joints[sfr::nui_joint::hand_right][2] == 2.2f, "the hand's depth is left as measured");
}

void a_moving_hand_moves_the_cursor_at_once() {
    sfr::MenuHandSteadying steadying;
    auto joints = skeleton(0.3f, 0.5f);
    steadying.steady(joints, true);
    // A slow movement: 2 cm a picture is dragged, no more than the rest circle behind.
    joints = skeleton(0.32f, 0.5f);
    steadying.steady(joints, true);
    require(near(joints[sfr::nui_joint::hand_right][0], 0.32f - sfr::MenuHandSteadying::default_rest_radius, 1e-5f),
            "a slow movement starts at once, a little behind");
    require(near(joints[sfr::nui_joint::wrist_right][0], joints[sfr::nui_joint::hand_right][0], 1e-5f),
            "the wrist moves with the hand");
    // A fast one is followed exactly.
    joints = skeleton(0.42f, 0.5f);
    steadying.steady(joints, true);
    require(joints[sfr::nui_joint::hand_right][0] == 0.42f, "a fast movement is followed with no lag");
}

void one_wrong_picture_does_not_throw_the_cursor() {
    sfr::MenuHandSteadying steadying;
    auto joints = skeleton(0.3f, 0.5f);
    steadying.steady(joints, true);
    joints = skeleton(-0.4f, 0.5f);
    steadying.steady(joints, true);
    require(near(joints[sfr::nui_joint::hand_right][0], 0.3f, 1e-5f), "a single picture across the screen is ignored");
    joints = skeleton(-0.4f, 0.5f);
    steadying.steady(joints, true);
    require(near(joints[sfr::nui_joint::hand_right][0], -0.4f, 1e-5f), "the next picture agreeing is followed");
}

void the_players_switch_turns_it_off_and_on() {
    sfr::MenuHandSteadying steadying;
    auto joints = skeleton(0.3f, 0.5f);
    steadying.steady(joints, true);
    sfr::set_menu_steadying(false);
    joints = skeleton(0.303f, 0.5f);
    steadying.steady(joints, true);
    require(joints[sfr::nui_joint::hand_right][0] == 0.303f, "switched off, the hands are left as measured");
    sfr::set_menu_steadying(true);
    joints = skeleton(0.303f, 0.5f);
    steadying.steady(joints, true);
    joints = skeleton(0.306f, 0.5f);
    steadying.steady(joints, true);
    require(near(joints[sfr::nui_joint::hand_right][0], 0.303f, 1e-5f), "switched on again, a still hand holds the cursor");
}

void racing_hands_pass_through() {
    sfr::MenuHandSteadying steadying;
    auto joints = skeleton(0.3f, 0.5f);
    steadying.steady(joints, true);
    joints = skeleton(0.303f, 0.5f);
    steadying.steady(joints, false);
    require(joints[sfr::nui_joint::hand_right][0] == 0.303f, "while racing the hands are gestures, untouched");
}
}

int main() {
    try {
        a_body_it_believes_passes_untouched();
        a_hand_the_model_cannot_see_is_held_on_the_arm();
        a_hold_ends_and_the_model_is_believed_again();
        a_bone_that_grows_is_a_wrong_point();
        a_jump_is_ignored_but_a_movement_is_followed();
        a_fast_but_possible_movement_is_not_held();
        it_can_be_turned_off();
        a_still_hand_holds_the_menu_cursor();
        a_moving_hand_moves_the_cursor_at_once();
        one_wrong_picture_does_not_throw_the_cursor();
        racing_hands_pass_through();
        the_players_switch_turns_it_off_and_on();
    } catch (const std::exception& error) {
        std::cerr << "pose_stability_test: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
