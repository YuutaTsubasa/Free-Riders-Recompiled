#include "pose_skeleton.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace sfr {
namespace {
using Point = std::array<float, 3>;

Point middle(const Point& a, const Point& b) {
    return {(a[0] + b[0]) * 0.5f, (a[1] + b[1]) * 0.5f, (a[2] + b[2]) * 0.5f};
}

Point between(const Point& from, const Point& to, float part) {
    return {from[0] + (to[0] - from[0]) * part, from[1] + (to[1] - from[1]) * part,
            from[2] + (to[2] - from[2]) * part};
}
}

bool pose_to_joints(const PoseLandmarks& given, uint32_t picture_width, uint32_t picture_height,
                    SkeletonJoints& joints, bool picture_is_mirrored, PoseMapping* mapping) {
    if (!picture_width || !picture_height) return false;
    using namespace pose_point;
    // A mirrored picture is turned back first: every point moves to the other
    // side, and the two sides change places, since a model shown a mirrored
    // person calls their right hand their left -- it has no way to know.
    PoseLandmarks landmarks = given;
    if (picture_is_mirrored) {
        for (auto& point : landmarks) point.x = float(picture_width) - point.x;
        for (const auto& pair : {std::pair{shoulder_left, shoulder_right}, {elbow_left, elbow_right},
                                 {wrist_left, wrist_right}, {hip_left, hip_right}, {knee_left, knee_right},
                                 {ankle_left, ankle_right}, {eye_left, eye_right}, {ear_left, ear_right}})
            std::swap(landmarks[pair.first], landmarks[pair.second]);
    }
    // The torso decides whether there is a body at all: the limbs are often
    // guessed at the edge of the picture, but these four are not.
    const float torso = (std::min)((std::min)(landmarks[shoulder_left].score, landmarks[shoulder_right].score),
                                   (std::min)(landmarks[hip_left].score, landmarks[hip_right].score));
    if (torso <= 0) return false;
    // Metres per picture pixel, so that the torso comes out the length the
    // emulated player's is (see pose_skeleton.h for why not the shoulders).
    const float torso_x = (landmarks[shoulder_left].x + landmarks[shoulder_right].x -
                           landmarks[hip_left].x - landmarks[hip_right].x) * 0.5f;
    const float torso_y = (landmarks[shoulder_left].y + landmarks[shoulder_right].y -
                           landmarks[hip_left].y - landmarks[hip_right].y) * 0.5f;
    const float torso_span = std::sqrt(torso_x * torso_x + torso_y * torso_y);
    if (torso_span < 4.0f) return false;  // too small to scale by
    float metres = pose_torso_length / torso_span;
    if (mapping) {
        if (mapping->scale > 0) metres = mapping->scale + (metres - mapping->scale) * 0.1f;
        mapping->scale = metres;
        // The leading foot is the nearer one, lower in the picture. Ankles
        // within a sixteenth of the torso of each other say nothing.
        const float ankles = landmarks[ankle_left].y - landmarks[ankle_right].y;
        const bool seen = landmarks[ankle_left].score > 0.3f && landmarks[ankle_right].score > 0.3f;
        if (seen && std::fabs(ankles) > torso_span / 16.0f) mapping->lead = ankles > 0 ? -1 : 1;
    }
    // The picture is mirrored: a camera faces the player, and the title
    // expects the sensor's own left and right.
    const Point centre{(landmarks[shoulder_left].x + landmarks[shoulder_right].x +
                        landmarks[hip_left].x + landmarks[hip_right].x) * 0.25f,
                       (landmarks[hip_left].y + landmarks[hip_right].y) * 0.5f, 0};
    const auto place = [&](uint32_t point) -> Point {
        return {(centre[0] - landmarks[point].x) * metres,
                (centre[1] - landmarks[point].y) * metres,
                pose_distance};
    };

    // The player's right is the sensor's +x, which is where the mirroring in
    // place() has already put the picture's left half. The model names the
    // sides from the body, not from the picture, so the names carry straight
    // across: right to right (nui_skeleton.cpp's emulated player has its right
    // shoulder at +0.18 too, and the title's cursor centre sits beside it).
    namespace joint = nui_joint;
    joints[joint::shoulder_right] = place(pose_point::shoulder_right);
    joints[joint::shoulder_left] = place(pose_point::shoulder_left);
    joints[joint::elbow_right] = place(pose_point::elbow_right);
    joints[joint::elbow_left] = place(pose_point::elbow_left);
    joints[joint::hand_right] = place(pose_point::wrist_right);
    joints[joint::hand_left] = place(pose_point::wrist_left);
    joints[joint::hip_right] = place(pose_point::hip_right);
    joints[joint::hip_left] = place(pose_point::hip_left);
    joints[joint::knee_right] = place(pose_point::knee_right);
    joints[joint::knee_left] = place(pose_point::knee_left);
    joints[joint::ankle_right] = place(pose_point::ankle_right);
    joints[joint::ankle_left] = place(pose_point::ankle_left);
    joints[joint::head] = place(nose);

    // The joints a NUI skeleton has and the model does not: the wrists sit
    // most of the way down the forearm, the feet just past the ankles, and
    // the spine between the hips and the shoulders.
    joints[joint::wrist_right] = between(joints[joint::elbow_right], joints[joint::hand_right], 0.9f);
    joints[joint::wrist_left] = between(joints[joint::elbow_left], joints[joint::hand_left], 0.9f);
    joints[joint::foot_right] = {joints[joint::ankle_right][0], joints[joint::ankle_right][1] - 0.05f, pose_distance - 0.08f};
    joints[joint::foot_left] = {joints[joint::ankle_left][0], joints[joint::ankle_left][1] - 0.05f, pose_distance - 0.08f};

    // How far one side is behind the other, from how much narrower than
    // their true width the shoulders and the hips look: the nearer side is
    // the leading one, and each arm and leg goes with its shoulder or hip.
    if (mapping && mapping->lead != 0) {
        const auto gap = [](float half_width, float seen_span) {
            const float width = half_width * 2.0f;
            return std::sqrt((std::max)(0.0f, width * width - seen_span * seen_span));
        };
        const float shoulders = gap(pose_shoulder_half_width,
                                    std::fabs(joints[joint::shoulder_right][0] - joints[joint::shoulder_left][0]));
        const float hips = gap(pose_hip_half_width, std::fabs(joints[joint::hip_right][0] - joints[joint::hip_left][0]));
        const float left_side = mapping->lead < 0 ? -0.5f : 0.5f;  // the left comes nearer when it leads
        for (const uint32_t j : {joint::shoulder_left, joint::elbow_left, joint::wrist_left, joint::hand_left})
            joints[j][2] += left_side * shoulders;
        for (const uint32_t j : {joint::shoulder_right, joint::elbow_right, joint::wrist_right, joint::hand_right})
            joints[j][2] -= left_side * shoulders;
        for (const uint32_t j : {joint::hip_left, joint::knee_left, joint::ankle_left, joint::foot_left})
            joints[j][2] += left_side * hips;
        for (const uint32_t j : {joint::hip_right, joint::knee_right, joint::ankle_right, joint::foot_right})
            joints[j][2] -= left_side * hips;
    }
    joints[joint::hip_center] = middle(joints[joint::hip_left], joints[joint::hip_right]);
    joints[joint::shoulder_center] = middle(joints[joint::shoulder_left], joints[joint::shoulder_right]);
    joints[joint::spine] = between(joints[joint::hip_center], joints[joint::shoulder_center], 0.5f);
    return true;
}
}
