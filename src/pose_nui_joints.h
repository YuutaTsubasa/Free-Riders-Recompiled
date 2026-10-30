#pragma once
#include "nui_skeleton.h"
#include "pose_estimator.h"

#include <array>
#include <cstdint>

namespace sfr {

// The twenty joints of a NUI skeleton from the pose model's points, once
// each point has been placed in the sensor's camera space (metres). Both
// motion devices that read a body with the pose model come through here: a
// camera (pose_to_joints, which places the points from the picture alone)
// and a Kinect on a Mac (kinect_depth_body, which places them on the depth
// picture), so the two name the joints the same way (motion_input.h).
//
// The model's thirteen body points carry across by name: its wrists are the
// hands and its nose the head. The joints it does not have are made from
// them: the wrists most of the way down the forearm, the feet just past the
// ankles, the spine between the hips and the shoulders. A made joint is as
// sure (0 not tracked, 1 inferred, 2 tracked) as the least sure of the
// points it is made from.
inline void pose_nui_joints(const std::array<std::array<float, 3>, pose_point::count>& points,
                            const std::array<uint32_t, pose_point::count>& states,
                            std::array<std::array<float, 3>, nui_joint_count>& joints,
                            std::array<uint32_t, nui_joint_count>& joint_states) {
    using Point = std::array<float, 3>;
    const auto middle = [](const Point& a, const Point& b) {
        return Point{(a[0] + b[0]) * 0.5f, (a[1] + b[1]) * 0.5f, (a[2] + b[2]) * 0.5f};
    };
    const auto between = [](const Point& from, const Point& to, float part) {
        return Point{from[0] + (to[0] - from[0]) * part, from[1] + (to[1] - from[1]) * part,
                     from[2] + (to[2] - from[2]) * part};
    };
    namespace joint = nui_joint;
    const auto set = [&](uint32_t to, uint32_t from) {
        joints[to] = points[from];
        joint_states[to] = states[from];
    };
    set(joint::shoulder_right, pose_point::shoulder_right);
    set(joint::shoulder_left, pose_point::shoulder_left);
    set(joint::elbow_right, pose_point::elbow_right);
    set(joint::elbow_left, pose_point::elbow_left);
    set(joint::hand_right, pose_point::wrist_right);
    set(joint::hand_left, pose_point::wrist_left);
    set(joint::hip_right, pose_point::hip_right);
    set(joint::hip_left, pose_point::hip_left);
    set(joint::knee_right, pose_point::knee_right);
    set(joint::knee_left, pose_point::knee_left);
    set(joint::ankle_right, pose_point::ankle_right);
    set(joint::ankle_left, pose_point::ankle_left);
    set(joint::head, pose_point::nose);
    const auto made = [&](uint32_t to, const Point& at, uint32_t a, uint32_t b) {
        joints[to] = at;
        joint_states[to] = joint_states[a] < joint_states[b] ? joint_states[a] : joint_states[b];
    };
    made(joint::wrist_right, between(joints[joint::elbow_right], joints[joint::hand_right], 0.9f), joint::elbow_right,
         joint::hand_right);
    made(joint::wrist_left, between(joints[joint::elbow_left], joints[joint::hand_left], 0.9f), joint::elbow_left,
         joint::hand_left);
    const auto foot = [](const Point& ankle) { return Point{ankle[0], ankle[1] - 0.05f, ankle[2] - 0.08f}; };
    made(joint::foot_right, foot(joints[joint::ankle_right]), joint::ankle_right, joint::ankle_right);
    made(joint::foot_left, foot(joints[joint::ankle_left]), joint::ankle_left, joint::ankle_left);
    made(joint::hip_center, middle(joints[joint::hip_left], joints[joint::hip_right]), joint::hip_left, joint::hip_right);
    made(joint::shoulder_center, middle(joints[joint::shoulder_left], joints[joint::shoulder_right]), joint::shoulder_left,
         joint::shoulder_right);
    made(joint::spine, between(joints[joint::hip_center], joints[joint::shoulder_center], 0.5f), joint::hip_center,
         joint::shoulder_center);
}

}
