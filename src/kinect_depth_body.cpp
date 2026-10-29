#include "kinect_depth_body.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace sfr {
namespace {
using Point = std::array<float, 3>;

// NuiImageCamera.h's nominal focal lengths (kinect_preview.cpp): depth at
// 320x240, colour at 640x480.
constexpr float depth_focal = 285.63f, colour_focal = 531.15f;
// A joint is inside the body, a little behind the surface the depth sees.
constexpr float torso_surface_to_joint = 0.07f, limb_surface_to_joint = 0.04f;

Point middle(const Point& a, const Point& b) { return {(a[0] + b[0]) * 0.5f, (a[1] + b[1]) * 0.5f, (a[2] + b[2]) * 0.5f}; }
Point between(const Point& from, const Point& to, float part) {
    return {from[0] + (to[0] - from[0]) * part, from[1] + (to[1] - from[1]) * part, from[2] + (to[2] - from[2]) * part};
}

// A colour pixel at a depth, in camera space (the runtime's mirrored
// picture: +x, the player's right, is the picture's right).
Point colour_pixel_point(float x, float y, float metres) {
    return {(x - 320.0f) * metres / colour_focal, (240.0f - y) * metres / colour_focal, metres};
}
}

std::array<float, 3> kinect_depth_pixel_point(float x, float y, float metres) {
    return {(x - 160.0f) * metres / depth_focal, (120.0f - y) * metres / depth_focal, metres};
}

std::array<float, 2> kinect_colour_to_depth_pixel(float x, float y) {
    return {160.0f + (x - 320.0f) * depth_focal / colour_focal, 120.0f + (y - 240.0f) * depth_focal / colour_focal};
}

float kinect_depth_at(const KinectDepthMillimetres& depth, float depth_x, float depth_y, int radius, float near,
                      float far) {
    if (depth.size() != size_t(kinect_depth_width) * kinect_depth_height) return 0;
    const int cx = int(std::lround(depth_x)), cy = int(std::lround(depth_y));
    std::vector<uint16_t> found;
    for (int y = cy - radius; y <= cy + radius; ++y)
        for (int x = cx - radius; x <= cx + radius; ++x) {
            if (x < 0 || y < 0 || x >= int(kinect_depth_width) || y >= int(kinect_depth_height)) continue;
            const uint16_t mm = depth[size_t(y) * kinect_depth_width + size_t(x)];
            if (mm && mm >= near * 1000.0f && mm <= far * 1000.0f) found.push_back(mm);
        }
    if (found.empty()) return 0;
    std::nth_element(found.begin(), found.begin() + found.size() / 2, found.end());
    return found[found.size() / 2] / 1000.0f;
}

bool kinect_depth_body(const PoseLandmarks& given, const KinectDepthMillimetres& depth,
                       const KinectDepthBodySettings& settings, SkeletonJoints& joints,
                       std::array<uint32_t, nui_joint_count>& joint_states) {
    using namespace pose_point;
    if (depth.size() != size_t(kinect_depth_width) * kinect_depth_height) return false;
    // The model names the sides from the body it sees, and in a mirrored
    // picture it sees a mirrored body: its right hand is the player's left.
    PoseLandmarks landmarks = given;
    for (const auto& pair : {std::pair{shoulder_left, shoulder_right}, {elbow_left, elbow_right},
                             {wrist_left, wrist_right}, {hip_left, hip_right}, {knee_left, knee_right},
                             {ankle_left, ankle_right}, {eye_left, eye_right}, {ear_left, ear_right}})
        std::swap(landmarks[pair.first], landmarks[pair.second]);
    for (const auto& point : landmarks)
        if (!point.has_world || !std::isfinite(point.world[0]) || !std::isfinite(point.world[1]) ||
            !std::isfinite(point.world[2]))
            return false;
    const float torso = (std::min)((std::min)(landmarks[shoulder_left].score, landmarks[shoulder_right].score),
                                   (std::min)(landmarks[hip_left].score, landmarks[hip_right].score));
    if (!(torso >= settings.min_score)) return false;

    // Where the torso is: its depth at the shoulders, the spine and the hips.
    const auto pixel = [&](uint32_t point) { return std::array<float, 2>{landmarks[point].x, landmarks[point].y}; };
    const auto pixel_middle = [](const std::array<float, 2>& a, const std::array<float, 2>& b) {
        return std::array<float, 2>{(a[0] + b[0]) * 0.5f, (a[1] + b[1]) * 0.5f};
    };
    const auto shoulders = pixel_middle(pixel(shoulder_left), pixel(shoulder_right));
    const auto hips = pixel_middle(pixel(hip_left), pixel(hip_right));
    std::vector<float> torso_depths;
    for (const auto& at : {shoulders, pixel_middle(shoulders, hips), hips}) {
        const auto place = kinect_colour_to_depth_pixel(at[0], at[1]);
        if (const float metres = kinect_depth_at(depth, place[0], place[1], 4, 0.5f, 4.5f); metres > 0)
            torso_depths.push_back(metres);
    }
    if (torso_depths.empty()) return false;
    std::sort(torso_depths.begin(), torso_depths.end());
    const float torso_depth = torso_depths[torso_depths.size() / 2] + torso_surface_to_joint;

    // The model's shape, placed with its hips there. Its world points are
    // the picture's way round (x the picture's right, y down, z away), and
    // the picture is the runtime's mirrored one, so x carries straight
    // across to camera space.
    const Point anchor = colour_pixel_point(hips[0], hips[1], torso_depth);
    const Point world_centre = middle(landmarks[hip_left].world, landmarks[hip_right].world);
    const auto shape = [&](uint32_t point) -> Point {
        const auto& world = landmarks[point].world;
        return {anchor[0] + (world[0] - world_centre[0]), anchor[1] - (world[1] - world_centre[1]),
                anchor[2] + (world[2] - world_centre[2])};
    };
    // Each point at its own depth when it is sure of itself and the depth
    // there agrees with the shape; the shape's place otherwise.
    std::array<Point, pose_point::count> points{};
    std::array<uint32_t, pose_point::count> states{};
    for (uint32_t point = 0; point < pose_point::count; ++point) {
        points[point] = shape(point);
        states[point] = 1;
        if (landmarks[point].score < settings.min_score) continue;
        const auto place = kinect_colour_to_depth_pixel(landmarks[point].x, landmarks[point].y);
        const float expected = points[point][2] - limb_surface_to_joint;
        const float metres = kinect_depth_at(depth, place[0], place[1], 2, expected - settings.joint_agreement,
                                             expected + settings.joint_agreement);
        if (metres <= 0) continue;
        points[point] = colour_pixel_point(landmarks[point].x, landmarks[point].y, metres + limb_surface_to_joint);
        states[point] = 2;
    }

    // The twenty NUI joints, as pose_to_joints names them from the model's.
    namespace joint = nui_joint;
    const auto set = [&](uint32_t to, uint32_t from) {
        joints[to] = points[from];
        joint_states[to] = states[from];
    };
    set(joint::shoulder_right, shoulder_right);
    set(joint::shoulder_left, shoulder_left);
    set(joint::elbow_right, elbow_right);
    set(joint::elbow_left, elbow_left);
    set(joint::hand_right, wrist_right);
    set(joint::hand_left, wrist_left);
    set(joint::hip_right, hip_right);
    set(joint::hip_left, hip_left);
    set(joint::knee_right, knee_right);
    set(joint::knee_left, knee_left);
    set(joint::ankle_right, ankle_right);
    set(joint::ankle_left, ankle_left);
    set(joint::head, nose);
    joints[joint::head][2] += 0.07f;  // the head's centre is behind the nose
    const auto derived = [&](uint32_t to, const Point& at, uint32_t a, uint32_t b) {
        joints[to] = at;
        joint_states[to] = (std::min)(joint_states[a], joint_states[b]);
    };
    derived(joint::wrist_right, between(joints[joint::elbow_right], joints[joint::hand_right], 0.9f), joint::elbow_right,
            joint::hand_right);
    derived(joint::wrist_left, between(joints[joint::elbow_left], joints[joint::hand_left], 0.9f), joint::elbow_left,
            joint::hand_left);
    derived(joint::foot_right,
            {joints[joint::ankle_right][0], joints[joint::ankle_right][1] - 0.05f, joints[joint::ankle_right][2] - 0.08f},
            joint::ankle_right, joint::ankle_right);
    derived(joint::foot_left,
            {joints[joint::ankle_left][0], joints[joint::ankle_left][1] - 0.05f, joints[joint::ankle_left][2] - 0.08f},
            joint::ankle_left, joint::ankle_left);
    derived(joint::hip_center, middle(joints[joint::hip_left], joints[joint::hip_right]), joint::hip_left, joint::hip_right);
    derived(joint::shoulder_center, middle(joints[joint::shoulder_left], joints[joint::shoulder_right]),
            joint::shoulder_left, joint::shoulder_right);
    derived(joint::spine, between(joints[joint::hip_center], joints[joint::shoulder_center], 0.5f), joint::hip_center,
            joint::shoulder_center);
    return true;
}

uint32_t kinect_cut_player(const KinectDepthMillimetres& depth, const SkeletonJoints& joints, uint8_t player,
                           const KinectPlayerCutSettings& settings, std::vector<uint8_t>& index) {
    constexpr size_t count = size_t(kinect_depth_width) * kinect_depth_height;
    if (depth.size() != count || !player || player > 6) return 0;
    if (index.size() != count) index.assign(count, 0);
    // The space the player may fill: the skeleton's box with a margin, down
    // to just above the floor under the lower ankle.
    Point low = joints[0], high = joints[0];
    for (const auto& at : joints)
        for (int axis = 0; axis < 3; ++axis) {
            low[axis] = (std::min)(low[axis], at[axis]);
            high[axis] = (std::max)(high[axis], at[axis]);
        }
    const float floor_y = (std::min)(joints[nui_joint::ankle_left][1], joints[nui_joint::ankle_right][1]) -
                          settings.floor_metres;
    const auto inside = [&](const Point& at) {
        return at[0] >= low[0] - settings.margin_metres && at[0] <= high[0] + settings.margin_metres &&
               at[1] >= floor_y && at[1] <= high[1] + settings.margin_metres &&
               at[2] >= low[2] - settings.margin_metres && at[2] <= high[2] + settings.margin_metres;
    };
    const auto point_at = [&](size_t i) {
        return kinect_depth_pixel_point(float(i % kinect_depth_width), float(i / kinect_depth_width), depth[i] / 1000.0f);
    };
    // Seeds: each joint's pixel, where the depth there is near the joint.
    std::vector<size_t> queue;
    std::vector<uint8_t> seen(count, 0);
    for (const auto& at : joints) {
        if (!(at[2] > 0.1f)) continue;
        const int x = int(std::lround(160.0f + at[0] * depth_focal / at[2]));
        const int y = int(std::lround(120.0f - at[1] * depth_focal / at[2]));
        if (x < 0 || y < 0 || x >= int(kinect_depth_width) || y >= int(kinect_depth_height)) continue;
        const size_t i = size_t(y) * kinect_depth_width + size_t(x);
        if (seen[i] || !depth[i] || std::fabs(depth[i] / 1000.0f - at[2]) > 0.3f || !inside(point_at(i))) continue;
        seen[i] = 1;
        queue.push_back(i);
    }
    // Grown across neighbours of nearly the same depth.
    const float step_mm = settings.step_metres * 1000.0f;
    uint32_t marked = 0;
    for (size_t next = 0; next < queue.size(); ++next) {
        const size_t i = queue[next];
        index[i] = player;
        ++marked;
        const size_t x = i % kinect_depth_width, y = i / kinect_depth_width;
        const size_t neighbours[4] = {x > 0 ? i - 1 : i, x + 1 < kinect_depth_width ? i + 1 : i,
                                      y > 0 ? i - kinect_depth_width : i, y + 1 < kinect_depth_height ? i + kinect_depth_width : i};
        for (const size_t n : neighbours) {
            if (n == i || seen[n] || !depth[n]) continue;
            if (std::fabs(float(depth[n]) - float(depth[i])) > step_mm) continue;
            if (!inside(point_at(n))) continue;
            seen[n] = 1;
            queue.push_back(n);
        }
    }
    return marked;
}

void kinect_depth_and_player(const KinectDepthMillimetres& depth, const std::vector<uint8_t>& index,
                             std::vector<uint8_t>& pixels) {
    const size_t count = depth.size();
    pixels.resize(count * 2);
    for (size_t i = 0; i < count; ++i) {
        const uint16_t mm = (std::min)(depth[i], uint16_t(8191));
        const uint16_t value = uint16_t(mm << 3 | (i < index.size() ? index[i] & 7 : 0));
        pixels[2 * i] = uint8_t(value);
        pixels[2 * i + 1] = uint8_t(value >> 8);
    }
}

void kinect_depth_halve(const uint16_t* depth_640, KinectDepthMillimetres& depth) {
    depth.assign(size_t(kinect_depth_width) * kinect_depth_height, 0);
    for (uint32_t y = 0; y < kinect_depth_height; ++y)
        for (uint32_t x = 0; x < kinect_depth_width; ++x) {
            uint16_t nearest = 0;
            for (uint32_t dy = 0; dy < 2; ++dy)
                for (uint32_t dx = 0; dx < 2; ++dx)
                    if (const uint16_t mm = depth_640[size_t(2 * y + dy) * 640 + 2 * x + dx]; mm && (!nearest || mm < nearest))
                        nearest = mm;
            depth[size_t(y) * kinect_depth_width + x] = nearest;
        }
}

}
