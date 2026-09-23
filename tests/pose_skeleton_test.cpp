#include "pose_skeleton.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool near(float a, float b, float slack = 0.02f) { return std::fabs(a - b) <= slack; }

// A body standing in the middle of a 640x480 picture, arms down.
sfr::PoseLandmarks standing() {
    using namespace sfr::pose_point;
    sfr::PoseLandmarks landmarks{};
    const auto put = [&](uint32_t point, float x, float y) { landmarks[point] = {x, y, 0.9f}; };
    put(nose, 320, 120);
    put(shoulder_left, 360, 170);   // the picture's left: the player's right
    put(shoulder_right, 280, 170);
    put(elbow_left, 370, 230);
    put(elbow_right, 270, 230);
    put(wrist_left, 375, 290);
    put(wrist_right, 265, 290);
    put(hip_left, 345, 300);
    put(hip_right, 295, 300);
    put(knee_left, 348, 380);
    put(knee_right, 292, 380);
    put(ankle_left, 350, 450);
    put(ankle_right, 290, 450);
    return landmarks;
}

void a_standing_body_becomes_a_skeleton() {
    sfr::SkeletonJoints joints{};
    require(sfr::pose_to_joints(standing(), 640, 480, joints), "a confident body maps");
    using namespace sfr::nui_joint;
    // The shoulders come out the width the emulated player's are, and the
    // picture's left shoulder is the skeleton's right.
    require(near(joints[shoulder_right][0], -sfr::pose_shoulder_half_width), "the picture's left is the player's right");
    require(near(joints[shoulder_left][0], sfr::pose_shoulder_half_width), "and the other way round");
    require(near(joints[shoulder_left][2], sfr::pose_distance), "every joint stands at one distance");
    // Up in the picture is up in camera space.
    require(joints[head][1] > joints[shoulder_center][1], "the head is above the shoulders");
    require(joints[shoulder_center][1] > joints[hip_center][1], "the shoulders above the hips");
    require(joints[hip_center][1] > joints[knee_left][1] && joints[knee_left][1] > joints[ankle_left][1],
            "the knees and ankles below them");
    require(near(joints[spine][1], (joints[hip_center][1] + joints[shoulder_center][1]) * 0.5f),
            "the spine sits between the hips and the shoulders");
    require(joints[foot_left][2] < joints[ankle_left][2], "the feet reach towards the camera");
    require(std::fabs(joints[wrist_left][1] - joints[hand_left][1]) <
                std::fabs(joints[elbow_left][1] - joints[hand_left][1]),
            "the wrist is nearer the hand than the elbow is");
}

void a_raised_arm_raises_the_hand() {
    sfr::PoseLandmarks raised = standing();
    raised[sfr::pose_point::wrist_left] = {380, 90, 0.9f};  // the player's right hand, up
    sfr::SkeletonJoints joints{};
    require(sfr::pose_to_joints(raised, 640, 480, joints), "the raised arm maps");
    require(joints[sfr::nui_joint::hand_right][1] > joints[sfr::nui_joint::shoulder_right][1],
            "the raised hand is above the shoulder");
}

void what_is_refused() {
    sfr::SkeletonJoints joints{};
    sfr::PoseLandmarks nothing{};
    require(!sfr::pose_to_joints(nothing, 640, 480, joints), "no confidence, no skeleton");
    require(!sfr::pose_to_joints(standing(), 0, 480, joints), "an empty picture is refused");
    sfr::PoseLandmarks squashed = standing();
    squashed[sfr::pose_point::shoulder_left].x = squashed[sfr::pose_point::shoulder_right].x;
    require(!sfr::pose_to_joints(squashed, 640, 480, joints), "shoulders at one point cannot be scaled by");
}
}

int main() {
    try {
        a_standing_body_becomes_a_skeleton();
        a_raised_arm_raises_the_hand();
        what_is_refused();
        std::cout << "Pose skeleton checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
