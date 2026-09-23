#include "pose_skeleton.h"

#include <algorithm>
#include <cmath>

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

bool pose_to_joints(const PoseLandmarks& landmarks, uint32_t picture_width, uint32_t picture_height,
                    SkeletonJoints& joints) {
    if (!picture_width || !picture_height) return false;
    using namespace pose_point;
    // The torso decides whether there is a body at all: the limbs are often
    // guessed at the edge of the picture, but these four are not.
    const float torso = (std::min)((std::min)(landmarks[shoulder_left].score, landmarks[shoulder_right].score),
                                   (std::min)(landmarks[hip_left].score, landmarks[hip_right].score));
    if (torso <= 0) return false;
    const float shoulder_span = std::fabs(landmarks[shoulder_left].x - landmarks[shoulder_right].x);
    if (shoulder_span < 1.0f) return false;  // too small to scale by

    // Metres per picture pixel, so that the shoulders come out the width the
    // emulated player's are. The picture is mirrored: a camera faces the
    // player, and the title expects the sensor's own left and right.
    const float metres = pose_shoulder_half_width * 2.0f / shoulder_span;
    const Point centre{(landmarks[shoulder_left].x + landmarks[shoulder_right].x +
                        landmarks[hip_left].x + landmarks[hip_right].x) * 0.25f,
                       (landmarks[hip_left].y + landmarks[hip_right].y) * 0.5f, 0};
    const auto place = [&](uint32_t point) -> Point {
        return {(centre[0] - landmarks[point].x) * metres,
                (centre[1] - landmarks[point].y) * metres,
                pose_distance};
    };

    // The model's left is the picture's left, which is the player's right.
    namespace joint = nui_joint;
    joints[joint::shoulder_right] = place(pose_point::shoulder_left);
    joints[joint::shoulder_left] = place(pose_point::shoulder_right);
    joints[joint::elbow_right] = place(pose_point::elbow_left);
    joints[joint::elbow_left] = place(pose_point::elbow_right);
    joints[joint::hand_right] = place(pose_point::wrist_left);
    joints[joint::hand_left] = place(pose_point::wrist_right);
    joints[joint::hip_right] = place(pose_point::hip_left);
    joints[joint::hip_left] = place(pose_point::hip_right);
    joints[joint::knee_right] = place(pose_point::knee_left);
    joints[joint::knee_left] = place(pose_point::knee_right);
    joints[joint::ankle_right] = place(pose_point::ankle_left);
    joints[joint::ankle_left] = place(pose_point::ankle_right);
    joints[joint::head] = place(nose);

    // The joints a NUI skeleton has and the model does not: the wrists sit
    // most of the way down the forearm, the feet just past the ankles, and
    // the spine between the hips and the shoulders.
    joints[joint::wrist_right] = between(joints[joint::elbow_right], joints[joint::hand_right], 0.9f);
    joints[joint::wrist_left] = between(joints[joint::elbow_left], joints[joint::hand_left], 0.9f);
    joints[joint::foot_right] = {joints[joint::ankle_right][0], joints[joint::ankle_right][1] - 0.05f, pose_distance - 0.08f};
    joints[joint::foot_left] = {joints[joint::ankle_left][0], joints[joint::ankle_left][1] - 0.05f, pose_distance - 0.08f};
    joints[joint::hip_center] = middle(joints[joint::hip_left], joints[joint::hip_right]);
    joints[joint::shoulder_center] = middle(joints[joint::shoulder_left], joints[joint::shoulder_right]);
    joints[joint::spine] = between(joints[joint::hip_center], joints[joint::shoulder_center], 0.5f);
    return true;
}
}
