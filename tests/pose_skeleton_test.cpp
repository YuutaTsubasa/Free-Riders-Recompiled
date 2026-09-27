#include "pose_skeleton.h"

#include <cmath>
#include <utility>
#include <iostream>
#include <stdexcept>
#include <limits>

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
    // A camera faces the player, so the player's right is the picture's left.
    put(shoulder_left, 360, 170);
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
    // The shoulders come out the width the emulated player's are, and on
    // the sides the emulated player's are: nui_skeleton.cpp puts the right
    // shoulder at +0.18, and the title's menu cursor is centred beside it.
    require(near(joints[shoulder_right][0], sfr::pose_shoulder_half_width), "the player's right shoulder is at +x");
    require(near(joints[shoulder_left][0], -sfr::pose_shoulder_half_width), "and the left one across from it");
    require(joints[hand_right][0] > 0 && joints[hand_left][0] < 0, "the hands are on their own sides");
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
    raised[sfr::pose_point::wrist_right] = {260, 90, 0.9f};  // the player's right hand, up
    sfr::SkeletonJoints joints{};
    require(sfr::pose_to_joints(raised, 640, 480, joints), "the raised arm maps");
    require(joints[sfr::nui_joint::hand_right][1] > joints[sfr::nui_joint::shoulder_right][1],
            "the raised hand is above the shoulder");
    require(joints[sfr::nui_joint::hand_left][1] < joints[sfr::nui_joint::shoulder_left][1],
            "the other hand stays down");
}

// A camera that mirrors is turned back: the same body, however it arrives,
// comes out on the same sides.
void a_mirrored_picture_comes_out_the_same_way_round() {
    using namespace sfr::pose_point;
    sfr::PoseLandmarks mirrored = standing();
    for (auto& point : mirrored) point.x = 640.0f - point.x;
    for (const auto& pair : {std::pair{shoulder_left, shoulder_right}, {elbow_left, elbow_right},
                             {wrist_left, wrist_right}, {hip_left, hip_right}, {knee_left, knee_right},
                             {ankle_left, ankle_right}})
        std::swap(mirrored[pair.first], mirrored[pair.second]);
    mirrored[wrist_left] = {640.0f - 260.0f, 90, 0.9f};  // the player's right hand, up, on the far side

    namespace joint = sfr::nui_joint;
    sfr::SkeletonJoints joints{};
    require(sfr::pose_to_joints(mirrored, 640, 480, joints, true), "the mirrored body maps");
    require(near(joints[joint::shoulder_right][0], sfr::pose_shoulder_half_width),
            "the right shoulder is still at +x");
    require(joints[joint::hand_right][1] > joints[joint::shoulder_right][1],
            "and the raised hand is still the right one");

    sfr::SkeletonJoints unturned{};
    require(sfr::pose_to_joints(mirrored, 640, 480, unturned, false), "taken as it is, it still maps");
    require(unturned[joint::hand_left][1] > unturned[joint::shoulder_left][1],
            "but then it is the other hand that is up");
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

sfr::PoseLandmarks standing_world() {
    auto landmarks = standing();
    for (auto& point : landmarks) {
        point.world = {(point.x - 320.0f) * 0.005f, (point.y - 300.0f) * 0.005f, 0.0f};
        point.has_world = true;
    }
    return landmarks;
}

void world_depth_survives_reaching_and_retracting() {
    namespace p = sfr::pose_point;
    namespace j = sfr::nui_joint;
    auto body = standing_world();
    body[p::shoulder_right].world[2] = -0.1f;
    body[p::shoulder_left].world[2] = -0.1f;
    body[p::elbow_right].world[2] = -0.25f;
    body[p::wrist_right].world[2] = -0.5f;
    body[p::ankle_right].world[2] = 0.2f;
    sfr::SkeletonJoints forward{}, back{};
    require(sfr::pose_to_joints(body, 640, 480, forward), "world body maps");
    require(near(forward[j::hand_right][2], 2.05f, 0.0001f), "forward hand retains measured depth");
    require(near(forward[j::wrist_right][2], 2.0725f, 0.0001f), "inferred wrist interpolates depth");
    require(near(forward[j::spine][2], 2.455f, 0.0001f), "spine interpolates torso depth");
    require(near(forward[j::foot_right][2], 2.6f, 0.0001f), "foot depth follows its ankle");
    require(near(forward[j::hip_center][0], 0, 0.0001f) && near(forward[j::hip_center][1], 0, 0.0001f) &&
            near(forward[j::hip_center][2], sfr::pose_distance, 0.0001f), "hips anchor the body in all axes");
    body[p::wrist_right].world[2] = 0.2f;
    require(sfr::pose_to_joints(body, 640, 480, back), "retracted body maps");
    require(near(back[j::hand_right][2], 2.68f, 0.0001f), "retracted hand retains measured depth");
}

void world_scale_uses_the_full_shoulder_distance() {
    namespace p = sfr::pose_point;
    auto body = standing_world();
    body[p::shoulder_left].world = {0.12f, -0.65f, 0.16f};
    body[p::shoulder_right].world = {-0.12f, -0.65f, -0.16f};
    // An edge-on body can have no image-space shoulder separation.
    body[p::shoulder_right].x = body[p::shoulder_left].x;
    body[p::wrist_right].world[2] = -0.5f;
    sfr::SkeletonJoints joints{}, translated{};
    require(sfr::pose_to_joints(body, 640, 480, joints), "rotated 3D shoulders still define scale");
    require(near(joints[sfr::nui_joint::hand_right][2], 2.05f, 0.0001f), "rotation does not amplify reaching depth");
    for (auto& point : body) {
        point.world[0] += 0.4f;
        point.world[1] -= 0.2f;
        point.world[2] += 0.3f;
    }
    require(sfr::pose_to_joints(body, 640, 480, translated), "translated world body maps");
    for (size_t joint = 0; joint < joints.size(); ++joint)
        for (size_t axis = 0; axis < 3; ++axis)
            require(near(joints[joint][axis], translated[joint][axis], 0.0001f), "world origin does not change the anchored skeleton");
}

void world_mirroring_preserves_body_identity_and_depth() {
    using namespace sfr::pose_point;
    auto body = standing_world();
    body[wrist_right].world = {-0.35f, -0.8f, -0.5f};
    auto mirrored = body;
    for (auto& point : mirrored) { point.x = 640.0f - point.x; point.world[0] = -point.world[0]; }
    for (const auto& pair : {std::pair{shoulder_left, shoulder_right}, {elbow_left, elbow_right},
                            {wrist_left, wrist_right}, {hip_left, hip_right}, {knee_left, knee_right},
                            {ankle_left, ankle_right}, {eye_left, eye_right}, {ear_left, ear_right}})
        std::swap(mirrored[pair.first], mirrored[pair.second]);
    sfr::SkeletonJoints normal{}, corrected{};
    require(sfr::pose_to_joints(body, 640, 480, normal), "world reference maps");
    require(sfr::pose_to_joints(mirrored, 640, 480, corrected, true), "mirrored world body maps");
    for (size_t joint = 0; joint < normal.size(); ++joint)
        for (size_t axis = 0; axis < 3; ++axis)
            require(near(normal[joint][axis], corrected[joint][axis], 0.0001f), "mirror correction preserves XYZ and body identity");
}

void invalid_world_does_not_replace_the_last_skeleton() {
    namespace p = sfr::pose_point;
    sfr::SkeletonJoints original{};
    for (auto& point : original) point = {7, 8, 9};
    const auto refused = [&](const sfr::PoseLandmarks& body) {
        auto joints = original;
        require(!sfr::pose_to_joints(body, 640, 480, joints), "invalid world pose is refused");
        require(joints == original, "invalid input leaves every output joint untouched");
    };
    for (const float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), 100.0f}) {
        auto body = standing_world();
        body[p::wrist_right].world[2] = invalid;
        refused(body);
    }
    auto body = standing_world();
    body[p::ear_left].has_world = false;
    refused(body);
    body = standing_world();
    body[p::shoulder_right].world = body[p::shoulder_left].world;
    refused(body);
    body = standing_world();
    body[p::hip_left].score = std::numeric_limits<float>::quiet_NaN();
    refused(body);
}
}

int main() {
    try {
        a_standing_body_becomes_a_skeleton();
        a_raised_arm_raises_the_hand();
        a_mirrored_picture_comes_out_the_same_way_round();
        what_is_refused();
        world_depth_survives_reaching_and_retracting();
        world_scale_uses_the_full_shoulder_distance();
        world_mirroring_preserves_body_identity_and_depth();
        invalid_world_does_not_replace_the_last_skeleton();
        std::cout << "Pose skeleton checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
