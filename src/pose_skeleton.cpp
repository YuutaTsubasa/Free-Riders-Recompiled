#include "pose_skeleton.h"
#include "pose_nui_joints.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace sfr {
namespace {
using Point = std::array<float, 3>;

Point middle(const Point& a, const Point& b) {
    return {(a[0] + b[0]) * 0.5f, (a[1] + b[1]) * 0.5f, (a[2] + b[2]) * 0.5f};
}
}

bool pose_to_joints(const PoseLandmarks& given, uint32_t picture_width, uint32_t picture_height,
                    SkeletonJoints& joints, bool picture_is_mirrored) {
    if (!picture_width || !picture_height) return false;
    using namespace pose_point;
    // A mirrored picture is turned back first: every point moves to the other
    // side, and the two sides change places, since a model shown a mirrored
    // person calls their right hand their left -- it has no way to know.
    PoseLandmarks landmarks = given;
    const bool has_world = std::any_of(landmarks.begin(), landmarks.end(),
                                     [](const PoseLandmark& point) { return point.has_world; });
    if (has_world) {
        // World models supply all points together. A partial or corrupt pose
        // must not silently fall back to a flat skeleton or replace good data.
        for (const auto& point : landmarks) {
            if (!point.has_world || !std::isfinite(point.score)) return false;
            for (const float axis : point.world)
                if (!std::isfinite(axis) || std::fabs(axis) > 10.0f) return false;
        }
    }
    if (picture_is_mirrored) {
        for (auto& point : landmarks) {
            point.x = float(picture_width) - point.x;
            if (has_world) point.world[0] = -point.world[0];
        }
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
    float shoulder_span = std::fabs(landmarks[shoulder_left].x - landmarks[shoulder_right].x);
    if (has_world) {
        const auto& left = landmarks[shoulder_left].world;
        const auto& right = landmarks[shoulder_right].world;
        shoulder_span = std::hypot(left[0] - right[0], left[1] - right[1], left[2] - right[2]);
        // Reject collapsed or implausible model bodies before scaling. Using
        // all three axes keeps a turned body's depth from being exaggerated.
        if (shoulder_span < 0.05f || shoulder_span > 2.0f) return false;
    } else if (shoulder_span < 1.0f) return false;  // too small to scale by

    // One uniform scale makes the shoulders the emulated player's width.
    // World models use metres; legacy models use picture pixels. A camera
    // faces the player, so image X/Y signs are reversed for game space.
    const float metres = pose_shoulder_half_width * 2.0f / shoulder_span;
    const Point centre = has_world ? middle(landmarks[hip_left].world, landmarks[hip_right].world)
        : Point{(landmarks[shoulder_left].x + landmarks[shoulder_right].x +
                        landmarks[hip_left].x + landmarks[hip_right].x) * 0.25f,
                       (landmarks[hip_left].y + landmarks[hip_right].y) * 0.5f, 0};
    const auto place = [&](uint32_t point) -> Point {
        if (has_world) {
            const auto& world = landmarks[point].world;
            return {(centre[0] - world[0]) * metres, (centre[1] - world[1]) * metres,
                    pose_distance + (world[2] - centre[2]) * metres};
        }
        return {(centre[0] - landmarks[point].x) * metres,
                (centre[1] - landmarks[point].y) * metres,
                pose_distance};
    };

    // The player's right is the sensor's +x, which is where the mirroring in
    // place() has already put the picture's left half. The model names the
    // sides from the body, not from the picture, so the names carry straight
    // across: right to right (nui_skeleton.cpp's emulated player has its right
    // shoulder at +0.18 too, and the title's cursor centre sits beside it).
    // The joints themselves are named as the Kinect on a Mac names them
    // (pose_nui_joints.h); a camera's are all taken as tracked.
    std::array<Point, pose_point::count> points{};
    std::array<uint32_t, pose_point::count> states{};
    for (uint32_t point = 0; point < pose_point::count; ++point) {
        points[point] = place(point);
        states[point] = nui_tracked;
    }
    std::array<uint32_t, nui_joint_count> joint_states{};
    pose_nui_joints(points, states, joints, joint_states);
    return true;
}
}
