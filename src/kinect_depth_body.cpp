#include "kinect_depth_body.h"
#include "pose_nui_joints.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
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

// The sampling radii were chosen on the v1's 320 pixels across: as many
// pixels' worth of the view on another camera.
int scaled_radius(const KinectDepthCamera& camera, int radius_at_320) {
    return (std::max)(1, int(std::lround(float(radius_at_320) * float(camera.width) / float(kinect_depth_width))));
}
}

std::array<float, 3> kinect_depth_pixel_point(float x, float y, float metres) {
    return kinect_depth_pixel_point(KinectDepthCamera{}, x, y, metres);
}

std::array<float, 3> kinect_depth_pixel_point(const KinectDepthCamera& camera, float x, float y, float metres) {
    return {(x - camera.centre_x) * metres / camera.focal_x, (camera.centre_y - y) * metres / camera.focal_y, metres};
}

std::array<float, 2> kinect_colour_to_depth_pixel(float x, float y) {
    return {160.0f + (x - 320.0f) * depth_focal / colour_focal, 120.0f + (y - 240.0f) * depth_focal / colour_focal};
}

float kinect_depth_at(const KinectDepthMillimetres& depth, float depth_x, float depth_y, int radius, float near,
                      float far) {
    return kinect_depth_at(depth, KinectDepthCamera{}, depth_x, depth_y, radius, near, far);
}

float kinect_depth_at(const KinectDepthMillimetres& depth, const KinectDepthCamera& camera, float depth_x,
                      float depth_y, int radius, float near, float far) {
    if (depth.size() != camera.pixels()) return 0;
    const int cx = int(std::lround(depth_x)), cy = int(std::lround(depth_y));
    std::vector<uint16_t> found;
    for (int y = cy - radius; y <= cy + radius; ++y)
        for (int x = cx - radius; x <= cx + radius; ++x) {
            if (x < 0 || y < 0 || x >= int(camera.width) || y >= int(camera.height)) continue;
            const uint16_t mm = depth[size_t(y) * camera.width + size_t(x)];
            if (mm && mm >= near * 1000.0f && mm <= far * 1000.0f) found.push_back(mm);
        }
    if (found.empty()) return 0;
    std::nth_element(found.begin(), found.begin() + found.size() / 2, found.end());
    return found[found.size() / 2] / 1000.0f;
}

void KinectColourDepthGrid::build(const int* colour_of_depth, const KinectDepthMillimetres& depth,
                                  const KinectDepthCamera& camera, uint32_t colour_width, uint32_t colour_height,
                                  uint32_t cell, bool flip) {
    cell_ = (std::max)(cell, 1u);
    columns_ = (colour_width + cell_ - 1) / cell_;
    rows_ = (colour_height + cell_ - 1) / cell_;
    depth_width_ = camera.width;
    nearest_.assign(size_t(columns_) * rows_, -1);
    millimetres_.assign(nearest_.size(), 0);
    if (!colour_of_depth || depth.size() != camera.pixels()) return;
    const int colour_pixels = int(colour_width * colour_height);
    for (size_t i = 0; i < camera.pixels(); ++i) {
        const int colour = colour_of_depth[i];
        if (colour < 0 || colour >= colour_pixels) continue;
        uint32_t depth_x = uint32_t(i % camera.width);
        const uint32_t depth_y = uint32_t(i / camera.width);
        uint32_t colour_x = uint32_t(colour) % colour_width;
        const uint32_t colour_y = uint32_t(colour) / colour_width;
        if (flip) {
            depth_x = camera.width - 1 - depth_x;
            colour_x = colour_width - 1 - colour_x;
        }
        const size_t at = size_t(depth_y) * camera.width + depth_x;
        const uint16_t mm = depth[at];
        if (!mm) continue;
        const size_t into = size_t(colour_y / cell_) * columns_ + colour_x / cell_;
        if (nearest_[into] < 0 || mm < millimetres_[into]) {
            nearest_[into] = int32_t(at);
            millimetres_[into] = mm;
        }
    }
}

bool KinectColourDepthGrid::find(float x, float y, float& depth_x, float& depth_y) const {
    if (!(x >= 0) || !(y >= 0) || !depth_width_) return false;
    const int column = int(x) / int(cell_), row = int(y) / int(cell_);
    if (column >= int(columns_) || row >= int(rows_)) return false;
    for (int ring = 0; ring <= 3; ++ring) {
        int32_t best = -1;
        uint16_t best_mm = 0;
        for (int r = row - ring; r <= row + ring; ++r)
            for (int c = column - ring; c <= column + ring; ++c) {
                if ((std::max)(std::abs(r - row), std::abs(c - column)) != ring) continue;
                if (r < 0 || c < 0 || r >= int(rows_) || c >= int(columns_)) continue;
                const size_t at = size_t(r) * columns_ + size_t(c);
                if (nearest_[at] >= 0 && (best < 0 || millimetres_[at] < best_mm)) {
                    best = nearest_[at];
                    best_mm = millimetres_[at];
                }
            }
        if (best >= 0) {
            depth_x = float(uint32_t(best) % depth_width_);
            depth_y = float(uint32_t(best) / depth_width_);
            return true;
        }
    }
    return false;
}

void kinect_depth_camera_points(const KinectDepthMillimetres& depth, const KinectDepthCamera& camera,
                                std::vector<float>& points) {
    points.assign(camera.pixels() * 3, 0.0f);
    if (depth.size() != camera.pixels()) return;
    for (size_t i = 0; i < camera.pixels(); ++i) {
        if (!depth[i]) continue;
        const Point at = kinect_depth_pixel_point(camera, float(i % camera.width), float(i / camera.width),
                                                  depth[i] / 1000.0f);
        points[3 * i] = at[0];
        points[3 * i + 1] = at[1];
        points[3 * i + 2] = at[2];
    }
}

bool kinect_depth_body(const PoseLandmarks& given, const KinectDepthMillimetres& depth,
                       const KinectDepthBodySettings& settings, SkeletonJoints& joints,
                       std::array<uint32_t, nui_joint_count>& joint_states) {
    const auto to_depth = [](float x, float y, float& depth_x, float& depth_y) {
        const auto place = kinect_colour_to_depth_pixel(x, y);
        depth_x = place[0];
        depth_y = place[1];
        return true;
    };
    return kinect_depth_body(given, depth, KinectDepthCamera{}, to_depth, settings, joints, joint_states);
}

bool kinect_depth_body(const PoseLandmarks& given, const KinectDepthMillimetres& depth,
                       const KinectDepthCamera& camera, const KinectColourToDepth& to_depth,
                       const KinectDepthBodySettings& settings, SkeletonJoints& joints,
                       std::array<uint32_t, nui_joint_count>& joint_states) {
    using namespace pose_point;
    if (depth.size() != camera.pixels() || !to_depth) return false;
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
        float x = 0, y = 0;
        if (!to_depth(at[0], at[1], x, y)) continue;
        if (const float metres = kinect_depth_at(depth, camera, x, y, scaled_radius(camera, 4), 0.5f, 4.5f); metres > 0)
            torso_depths.push_back(metres);
    }
    float hips_x = 0, hips_y = 0;
    if (torso_depths.empty() || !to_depth(hips[0], hips[1], hips_x, hips_y)) return false;
    std::sort(torso_depths.begin(), torso_depths.end());
    const float torso_depth = torso_depths[torso_depths.size() / 2] + torso_surface_to_joint;

    // The model's shape, placed with its hips there. Its world points are
    // the picture's way round (x the picture's right, y down, z away), and
    // the picture is the runtime's mirrored one, so x carries straight
    // across to camera space.
    const Point anchor = kinect_depth_pixel_point(camera, hips_x, hips_y, torso_depth);
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
        float x = 0, y = 0;
        if (!to_depth(landmarks[point].x, landmarks[point].y, x, y)) continue;
        const float expected = points[point][2] - limb_surface_to_joint;
        const float metres = kinect_depth_at(depth, camera, x, y, scaled_radius(camera, 2),
                                             expected - settings.joint_agreement, expected + settings.joint_agreement);
        if (metres <= 0) continue;
        points[point] = kinect_depth_pixel_point(camera, x, y, metres + limb_surface_to_joint);
        states[point] = 2;
    }

    // The twenty NUI joints, named as a camera's are (pose_nui_joints.h).
    // The head's centre is behind the nose, which is all the model sees of it.
    pose_nui_joints(points, states, joints, joint_states);
    joints[nui_joint::head][2] += 0.07f;
    return true;
}

uint32_t kinect_cut_player(const KinectDepthMillimetres& depth, const SkeletonJoints& joints, uint8_t player,
                           const KinectPlayerCutSettings& settings, std::vector<uint8_t>& index) {
    return kinect_cut_player(depth, KinectDepthCamera{}, joints, player, settings, index);
}

uint32_t kinect_cut_player(const KinectDepthMillimetres& depth, const KinectDepthCamera& camera,
                           const SkeletonJoints& joints, uint8_t player, const KinectPlayerCutSettings& settings,
                           std::vector<uint8_t>& index) {
    const size_t count = camera.pixels();
    const uint32_t width = camera.width, height = camera.height;
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
        return kinect_depth_pixel_point(camera, float(i % width), float(i / width), depth[i] / 1000.0f);
    };
    // Seeds: each joint's pixel, where the depth there is near the joint.
    std::vector<size_t> queue;
    std::vector<uint8_t> seen(count, 0);
    for (const auto& at : joints) {
        if (!(at[2] > 0.1f)) continue;
        const int x = int(std::lround(camera.centre_x + at[0] * camera.focal_x / at[2]));
        const int y = int(std::lround(camera.centre_y - at[1] * camera.focal_y / at[2]));
        if (x < 0 || y < 0 || x >= int(width) || y >= int(height)) continue;
        const size_t i = size_t(y) * width + size_t(x);
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
        const size_t x = i % width, y = i / width;
        const size_t neighbours[4] = {x > 0 ? i - 1 : i, x + 1 < width ? i + 1 : i, y > 0 ? i - width : i,
                                      y + 1 < height ? i + width : i};
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
