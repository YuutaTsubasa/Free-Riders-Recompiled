#include "pose_skeleton.h"

#include <cmath>
#include <utility>
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
    // A camera faces the player, so the player's right is the picture's left.
    // Shoulders about three quarters as wide as the torso is long, as
    // people's are (the torso, 130 px, is what the picture is scaled by).
    put(shoulder_left, 369, 170);
    put(shoulder_right, 271, 170);
    put(elbow_left, 379, 230);
    put(elbow_right, 261, 230);
    put(wrist_left, 384, 290);
    put(wrist_right, 256, 290);
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
    for (const uint32_t point : {sfr::pose_point::hip_left, sfr::pose_point::hip_right})
        squashed[point].y = squashed[sfr::pose_point::shoulder_left].y;
    require(!sfr::pose_to_joints(squashed, 640, 480, joints), "hips level with the shoulders cannot be scaled by");
}

// A board is ridden side-on: from the front the shoulders are one behind
// the other. The body keeps its height, which the shoulders alone lost.
void a_side_on_rider_keeps_its_size() {
    using namespace sfr::pose_point;
    sfr::PoseLandmarks side = standing();
    side[shoulder_left].x = 322;
    side[shoulder_right].x = 318;
    side[hip_left].x = 322;
    side[hip_right].x = 318;
    sfr::SkeletonJoints front{}, turned{};
    require(sfr::pose_to_joints(standing(), 640, 480, front), "facing the camera maps");
    require(sfr::pose_to_joints(side, 640, 480, turned), "side-on maps too");
    namespace joint = sfr::nui_joint;
    require(near(turned[joint::head][1] - turned[joint::ankle_left][1],
                 front[joint::head][1] - front[joint::ankle_left][1]),
            "side-on, the body is as tall as facing the camera");
    require(near(front[joint::shoulder_center][1] - front[joint::hip_center][1], sfr::pose_torso_length),
            "the torso comes out the emulated player's length");

    // The scale follows slowly: a sudden half-length torso (a bend towards
    // the camera) moves it a tenth of the way.
    sfr::PoseMapping mapping;
    require(sfr::pose_to_joints(standing(), 640, 480, front, false, &mapping) && mapping.scale > 0, "the scale starts");
    const float settled = mapping.scale;
    sfr::PoseLandmarks bent = standing();
    for (const uint32_t point : {shoulder_left, shoulder_right}) bent[point].y = 235;
    require(sfr::pose_to_joints(bent, 640, 480, turned, false, &mapping), "a bent body maps");
    require(near(mapping.scale, settled * 1.1f, settled * 0.01f), "one picture moves the scale a tenth of the way");
}

// How far the rider has turned: side-on, one shoulder is a shoulder's width
// behind the other; opened towards the screen, less; facing it, not at all.
// The leading (nearer) side is the one whose ankle is lower in the picture.
void a_turn_gives_the_shoulders_depth() {
    using namespace sfr::pose_point;
    namespace joint = sfr::nui_joint;
    const auto rider = [](float shoulder_half_span, float left_ankle_lower) {
        sfr::PoseLandmarks body = standing();
        body[shoulder_left].x = 320 + shoulder_half_span;
        body[shoulder_right].x = 320 - shoulder_half_span;
        body[hip_left].x = 320 + shoulder_half_span * 0.5f;
        body[hip_right].x = 320 - shoulder_half_span * 0.5f;
        body[ankle_left].y = 450 + left_ankle_lower;
        return body;
    };
    const auto gap = [](const sfr::SkeletonJoints& j) {
        return j[joint::shoulder_right][2] - j[joint::shoulder_left][2];  // > 0: the left is nearer
    };
    sfr::SkeletonJoints joints{};
    sfr::PoseMapping regular;
    require(sfr::pose_to_joints(rider(2, 20), 640, 480, joints, false, &regular) && regular.lead == -1,
            "the lower left ankle makes the left the leading side");
    require(near(gap(joints), 2 * sfr::pose_shoulder_half_width, 0.03f),
            "side-on, the right shoulder is a shoulder's width behind the left");
    require(near(joints[joint::shoulder_center][2], sfr::pose_distance), "the body stays where it stood");
    require(joints[joint::hand_left][2] < joints[joint::hand_right][2], "each arm goes with its shoulder");
    // Opened a little towards the screen: the shoulders look wider.
    require(sfr::pose_to_joints(rider(24, 0), 640, 480, joints, false, &regular) && regular.lead == -1,
            "level ankles keep the leading side");
    const float opened = gap(joints);
    require(opened > 0.1f && opened < 2 * sfr::pose_shoulder_half_width - 0.03f, "opened, the gap is smaller");
    // Facing the screen, the shoulders look their true width: no gap.
    require(sfr::pose_to_joints(standing(), 640, 480, joints, false, &regular) && near(gap(joints), 0.0f, 0.02f),
            "facing the camera, both shoulders are at one distance");
    // A kick boost lifting the front foot high: its ankle rises far above
    // the other, which must not read as the far foot.
    sfr::PoseLandmarks kick = rider(2, 0);
    kick[ankle_left].y = 450 - 70;
    kick[knee_left].y = 300;
    for (int i = 0; i < 10; ++i) require(sfr::pose_to_joints(kick, 640, 480, joints, false, &regular), "a kick maps");
    require(regular.lead == -1, "a lifted foot does not change the leading side");
    // Paddling: the back foot slides nearer, so lower, for longer than a few
    // pictures, but the shoulders stay side-on: no switch. (Riding side-on
    // for a while first, so the facing picture above is long past.)
    for (int i = 0; i < 50; ++i) require(sfr::pose_to_joints(rider(2, 0), 640, 480, joints, false, &regular), "riding maps");
    for (int i = 0; i < 60; ++i) require(sfr::pose_to_joints(rider(2, -20), 640, 480, joints, false, &regular), "a paddle maps");
    require(regular.lead == -1, "a paddling foot sliding nearer is not a turn");
    // Turned round (switch): the shoulders pass through facing the camera,
    // then the right ankle is lower, the right leads once a few agree.
    require(sfr::pose_to_joints(standing(), 640, 480, joints, false, &regular), "the turn passes the front");
    for (uint32_t i = 1; i < sfr::PoseMapping::pictures_to_turn; ++i) {
        require(sfr::pose_to_joints(rider(2, -20), 640, 480, joints, false, &regular), "a switch maps");
        require(regular.lead == -1, "a picture or two of a turn is not a switch yet");
    }
    require(sfr::pose_to_joints(rider(2, -20), 640, 480, joints, false, &regular) && regular.lead == 1 &&
                gap(joints) < -0.3f,
            "after a switch the right shoulder is the nearer one");
    // Until a leading side is known, nothing is given depth.
    sfr::PoseMapping unknown;
    require(sfr::pose_to_joints(rider(2, 0), 640, 480, joints, false, &unknown) && unknown.lead == 0 &&
                near(gap(joints), 0.0f, 0.001f),
            "with the ankles level from the start, the body stays flat");
}
}

int main() {
    try {
        a_standing_body_becomes_a_skeleton();
        a_raised_arm_raises_the_hand();
        a_mirrored_picture_comes_out_the_same_way_round();
        what_is_refused();
        a_side_on_rider_keeps_its_size();
        a_turn_gives_the_shoulders_depth();
        std::cout << "Pose skeleton checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
