// The skeleton and player index made from a Kinect v1's cameras alone
// (kinect_depth_body.h), on a scene drawn here: a wall 3.5 m away, the floor
// 1 m below the sensor, and a player standing 2 m away with the right arm out
// to the side, beyond the depth the body's box gives (so its depth lands on
// the wall and the model's shape has to place it).
#include "kinect_depth_body.h"
#include "kinect_preview.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using Point = std::array<float, 3>;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

float distance(const Point& a, const Point& b) { return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]); }

constexpr float body_front = 2.0f, wall = 3.5f, sensor_height = 1.0f;

// The player's body seen as a box: from the shins to the head, shoulders wide.
bool in_body(float x, float y) { return x >= -0.22f && x <= 0.22f && y >= -0.85f && y <= 0.75f; }

sfr::KinectDepthMillimetres scene() {
    sfr::KinectDepthMillimetres depth(size_t(sfr::kinect_depth_width) * sfr::kinect_depth_height);
    for (uint32_t v = 0; v < sfr::kinect_depth_height; ++v)
        for (uint32_t u = 0; u < sfr::kinect_depth_width; ++u) {
            float metres = wall;
            // The floor: a ray going down meets it before the wall.
            if (v > 120) metres = (std::min)(metres, sensor_height * 285.63f / float(v - 120));
            const Point at_body = sfr::kinect_depth_pixel_point(float(u), float(v), body_front);
            if (in_body(at_body[0], at_body[1])) metres = body_front;
            depth[size_t(v) * sfr::kinect_depth_width + u] = uint16_t(std::lround(metres * 1000.0f));
        }
    return depth;
}

// The player's joints, in camera space: at the body's depth plus the joints'
// place inside it, the right arm straight out at shoulder height.
std::array<Point, sfr::pose_point::count> truth() {
    using namespace sfr::pose_point;
    const float z = body_front + 0.06f;
    std::array<Point, count> p{};
    p[nose] = {0.0f, 0.62f, body_front};
    p[eye_left] = {-0.03f, 0.65f, z};
    p[eye_right] = {0.03f, 0.65f, z};
    p[ear_left] = {-0.07f, 0.63f, z};
    p[ear_right] = {0.07f, 0.63f, z};
    // Body names: the player's right is camera +x.
    p[shoulder_right] = {0.18f, 0.45f, z};
    p[shoulder_left] = {-0.18f, 0.45f, z};
    p[elbow_right] = {0.45f, 0.45f, z};    // the arm out to the side: depth there is the wall
    p[wrist_right] = {0.70f, 0.45f, z};
    p[elbow_left] = {-0.20f, 0.18f, z};
    p[wrist_left] = {-0.20f, -0.05f, z};
    p[hip_right] = {0.10f, 0.0f, z};
    p[hip_left] = {-0.10f, 0.0f, z};
    p[knee_right] = {0.10f, -0.45f, z};
    p[knee_left] = {-0.10f, -0.45f, z};
    p[ankle_right] = {0.10f, -0.80f, z};
    p[ankle_left] = {-0.10f, -0.80f, z};
    return p;
}

// What the pose model would say about that player in the runtime's mirrored
// colour picture: the points where the lens puts them, the world shape the
// picture's way round, and the sides swapped (it sees a mirrored body).
sfr::PoseLandmarks landmarks_for(const std::array<Point, sfr::pose_point::count>& joints) {
    using namespace sfr::pose_point;
    sfr::PoseLandmarks out{};
    const Point centre = {(joints[hip_left][0] + joints[hip_right][0]) * 0.5f,
                          (joints[hip_left][1] + joints[hip_right][1]) * 0.5f,
                          (joints[hip_left][2] + joints[hip_right][2]) * 0.5f};
    for (uint32_t i = 0; i < count; ++i) {
        const auto place = sfr::kinect_colour_point(joints[i]);
        out[i].x = place.x;
        out[i].y = place.y;
        out[i].score = 0.95f;
        out[i].world = {joints[i][0] - centre[0], -(joints[i][1] - centre[1]), joints[i][2] - centre[2]};
        out[i].has_world = true;
    }
    for (const auto& pair : {std::pair{shoulder_left, shoulder_right}, {elbow_left, elbow_right},
                             {wrist_left, wrist_right}, {hip_left, hip_right}, {knee_left, knee_right},
                             {ankle_left, ankle_right}, {eye_left, eye_right}, {ear_left, ear_right}})
        std::swap(out[pair.first], out[pair.second]);
    return out;
}

void lens() {
    const Point at = {0.3f, -0.2f, 2.0f};
    const auto in_depth = sfr::kinect_depth_point(at);
    const Point back = sfr::kinect_depth_pixel_point(in_depth.x, in_depth.y, at[2]);
    require(distance(at, back) < 1e-4f, "a depth pixel goes back to its point");
    const auto in_colour = sfr::kinect_colour_point(at);
    const auto mapped = sfr::kinect_colour_to_depth_pixel(in_colour.x, in_colour.y);
    require(std::fabs(mapped[0] - in_depth.x) < 1e-3f && std::fabs(mapped[1] - in_depth.y) < 1e-3f,
            "a colour pixel lands where its point does in the depth picture");
}

void depth_sampling() {
    const auto depth = scene();
    require(std::fabs(sfr::kinect_depth_at(depth, 160, 100, 2, 0.5f, 4.5f) - body_front) < 1e-3f, "the body's depth");
    require(std::fabs(sfr::kinect_depth_at(depth, 5, 60, 2, 0.5f, 4.5f) - wall) < 1e-3f, "the wall's depth");
    require(sfr::kinect_depth_at(depth, 5, 60, 2, 0.5f, 3.0f) == 0, "nothing in range: none");
}

void skeleton() {
    const auto depth = scene();
    const auto joints = truth();
    sfr::SkeletonJoints made{};
    std::array<uint32_t, sfr::nui_joint_count> states{};
    require(sfr::kinect_depth_body(landmarks_for(joints), depth, {}, made, states), "a body is made");
    namespace joint = sfr::nui_joint;
    using namespace sfr::pose_point;
    const auto near = [&](uint32_t nui, uint32_t point, float within, const char* name) {
        const float off = distance(made[nui], joints[point]);
        if (off > within) throw std::runtime_error(std::string(name) + " is " + std::to_string(off) + " m off");
    };
    near(joint::shoulder_right, shoulder_right, 0.03f, "the right shoulder");
    near(joint::shoulder_left, shoulder_left, 0.03f, "the left shoulder");
    near(joint::hip_right, hip_right, 0.03f, "the right hip");
    near(joint::knee_left, knee_left, 0.03f, "the left knee");
    require(made[joint::shoulder_right][0] > 0 && made[joint::hand_left][0] < 0,
            "the player's right side is +x: the mirrored model's sides are swapped back");
    require(states[joint::shoulder_right] == 2 && states[joint::hip_left] == 2, "the torso at its own depth");
    // The arm out to the side: its depth is the wall's, far from the shape,
    // so the shape places it.
    require(states[joint::hand_right] == 1 && states[joint::elbow_right] == 1, "the arm out of the body box: shape");
    near(joint::hand_right, wrist_right, 0.05f, "the right hand");
    require(std::fabs(made[joint::hip_center][2] - (body_front + 0.06f)) < 0.03f, "the hips at the body's depth");
    // No torso, no body.
    auto unsure = landmarks_for(joints);
    unsure[shoulder_left].score = 0.1f;
    require(!sfr::kinect_depth_body(unsure, depth, {}, made, states), "an unsure torso is no body");
    auto flat = landmarks_for(joints);
    flat[nose].has_world = false;
    require(!sfr::kinect_depth_body(flat, depth, {}, made, states), "no world shape, no body");
}

void player_cut() {
    const auto depth = scene();
    sfr::SkeletonJoints made{};
    std::array<uint32_t, sfr::nui_joint_count> states{};
    require(sfr::kinect_depth_body(landmarks_for(truth()), depth, {}, made, states), "a body to cut");
    std::vector<uint8_t> index;
    const uint32_t marked = sfr::kinect_cut_player(depth, made, 1, {}, index);
    uint32_t body = 0, wrong = 0;
    for (size_t i = 0; i < depth.size(); ++i) {
        const bool is_body = depth[i] == uint16_t(body_front * 1000.0f);
        body += is_body;
        if (index[i] && !is_body) ++wrong;
        if (index[i] && index[i] != 1) ++wrong;
    }
    require(wrong == 0, "nothing but the body is marked (" + std::to_string(wrong) + " pixels)");
    require(marked > body * 9 / 10, "most of the body is marked (" + std::to_string(marked) + " of " +
                                        std::to_string(body) + ")");
    std::vector<uint8_t> pixels;
    sfr::kinect_depth_and_player(depth, index, pixels);
    const size_t centre = size_t(110) * sfr::kinect_depth_width + 160;
    const uint16_t value = uint16_t(pixels[2 * centre] | pixels[2 * centre + 1] << 8);
    require(value == uint16_t(2000 << 3 | 1), "the game's pixel: millimetres << 3 | player");
}

void halving() {
    std::vector<uint16_t> big(640 * 480, 0);
    big[0] = 2100;
    big[1] = 2000;  // nearest of the 2x2
    big[640] = 0;   // nothing measured is not the nearest
    big[2 * 640 + 2] = 1500;
    sfr::KinectDepthMillimetres small;
    sfr::kinect_depth_halve(big.data(), small);
    require(small.size() == 320 * 240 && small[0] == 2000 && small[321] == 1500 && small[1] == 0,
            "each 2x2's nearest measured depth");
}
}

int main() {
    try {
        lens();
        depth_sampling();
        skeleton();
        player_cut();
        halving();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Kinect depth body checks passed\n";
    return 0;
}
