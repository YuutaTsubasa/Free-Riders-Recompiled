#include "stereo_view.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using sfr::stereo::Vec3;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool near(float a, float b, float tolerance = 1e-5f) { return std::fabs(a - b) <= tolerance; }
bool near(Vec3 a, Vec3 b, float tolerance = 1e-5f) {
    return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance);
}

// A row-major look-at for row vectors (D3DXMatrixLookAtLH's layout): the
// camera's axes are the first three columns.
void look_at(Vec3 eye, Vec3 target, Vec3 up, float m[16]) {
    const Vec3 z = sfr::stereo::normalized(target - eye);
    const Vec3 x = sfr::stereo::normalized(sfr::stereo::cross(up, z));
    const Vec3 y = sfr::stereo::cross(z, x);
    const float values[16] = {x.x, y.x, z.x, 0, x.y, y.y, z.y, 0, x.z, y.z, z.z, 0,
                              -sfr::stereo::dot(x, eye), -sfr::stereo::dot(y, eye), -sfr::stereo::dot(z, eye), 1};
    for (int i = 0; i < 16; ++i) m[i] = values[i];
}
// A point through a row-major matrix (row vector, w = 1).
Vec3 transform(Vec3 p, const float m[16]) {
    return {p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12], p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13],
            p.x * m[2] + p.y * m[6] + p.z * m[10] + m[14]};
}

void right_of_a_view() {
    float m[16];
    look_at({0, 0, 0}, {0, 0, 1}, {0, 1, 0}, m);
    require(near(sfr::stereo::view_right(m), {1, 0, 0}), "looking down +z with +y up, the right is +x");
    look_at({3, 2, 1}, {3, 2, -5}, {0, 1, 0}, m);
    require(near(sfr::stereo::view_right(m), {-1, 0, 0}), "looking down -z, the right is -x");
    // Whatever the view, its right maps to the view's +x and nothing else.
    const Vec3 eye{1.5f, -2, 7}, target{-4, 3, 2.5f};
    look_at(eye, target, {0, 1, 0}, m);
    const Vec3 right = sfr::stereo::view_right(m);
    require(near(sfr::stereo::length(right), 1), "the right is a unit vector");
    const Vec3 moved = transform(eye + right, m);
    require(near(moved, {1, 0, 0}, 1e-4f), "a step to the right of the eye is +x in the view");
    require(near(sfr::stereo::dot(right, target - eye), 0, 1e-4f), "the right is square to the view direction");
}

void eye_offsets() {
    const Vec3 right{0, 0, 1};
    require(near(sfr::stereo::eye_offset(right, 0.065f, 0), {0, 0, -0.0325f}), "the left eye is half the separation to the left");
    require(near(sfr::stereo::eye_offset(right, 0.065f, 1), {0, 0, 0.0325f}), "the right eye half to the right");
    require(near(sfr::stereo::eye_offset(right, 0, 0), {0, 0, 0}) && near(sfr::stereo::eye_offset(right, 0, 1), {0, 0, 0}),
            "no separation leaves both eyes at the camera");
    // Moving eye and target alike keeps the eyes' views parallel: the same
    // view matrix but for its translation, by the offset along view x.
    float centre[16], left[16];
    const Vec3 eye{2, 1, -3}, target{0, 1, 4}, up{0, 1, 0};
    look_at(eye, target, up, centre);
    const Vec3 offset = sfr::stereo::eye_offset(sfr::stereo::view_right(centre), 0.5f, 0);
    look_at(eye + offset, target + offset, up, left);
    for (int i = 0; i < 12; ++i) require(near(centre[i], left[i]), "the eyes look the same way");
    require(near(left[12] - centre[12], 0.25f, 1e-4f), "the world moves right in the left eye's view");
    require(near(left[13], centre[13], 1e-4f) && near(left[14], centre[14], 1e-4f), "and nowhere else");
}

void halves() {
    const auto l = sfr::stereo::eye_span(0, 880, 0), r = sfr::stereo::eye_span(0, 880, 1);
    require(l.x == 0 && l.width == 440 && r.x == 440 && r.width == 440, "an 880 view splits at 440 (the two-player halves)");
    const auto l2 = sfr::stereo::eye_span(10, 1211, 0), r2 = sfr::stereo::eye_span(10, 1211, 1);
    require(l2.x == 10 && l2.width == 605 && r2.x == 615 && r2.width == 606, "an odd width: halves meet, the right a pixel wider");
    require(l2.x + l2.width == r2.x && r2.x + r2.width == 10 + 1211, "the halves cover the view exactly");
}

void aspects() {
    require(near(sfr::stereo::eye_aspect(16.0f / 9.0f, 880, 440), 8.0f / 9.0f), "half of 16:9 is 8:9");
    require(near(sfr::stereo::eye_aspect(2.3333f, 1210, 605), 2.3333f / 2), "half of a 21:9 race view");
    require(near(sfr::stereo::eye_aspect(1.5f, 0, 0), 1.5f), "no width keeps the aspect");
}

void separations() {
    using sfr::stereo::default_separation;
    require(near(sfr::stereo::parse_separation(nullptr), default_separation), "absent: the default");
    require(near(sfr::stereo::parse_separation(""), default_separation), "empty: the default");
    require(near(sfr::stereo::parse_separation("0"), 0), "0: both eyes at the camera");
    require(near(sfr::stereo::parse_separation("0.3"), 0.3f), "a number");
    require(near(sfr::stereo::parse_separation("-1"), 0), "negative: 0");
    require(near(sfr::stereo::parse_separation("1000"), 10), "at most 10");
    require(near(sfr::stereo::parse_separation("wide"), default_separation), "not a number: the default");
    require(near(sfr::stereo::parse_separation("0.3m"), default_separation), "trailing text: the default");
}

void head() {
    // Racer at (5,0,5) heading +x, up +y: the eye is above and ahead.
    auto pose = sfr::stereo::head_pose({5, 0, 5}, {2, 0, 0}, {0, 1, 0}, 1.5f, 0.3f);
    require(near(pose.eye, {5.3f, 1.5f, 5}), "eye above the racer and ahead of its head");
    require(near(pose.target, {6.3f, 1.5f, 5}), "looking along the heading");
    require(near(pose.up, {0, 1, 0}), "the racer's up");
    // Pitched down a slope: up is made square to forward.
    pose = sfr::stereo::head_pose({0, 0, 0}, {1, -1, 0}, {0, 1, 0}, 1, 0);
    require(near(sfr::stereo::dot(pose.up, pose.target - pose.eye), 0, 1e-5f), "up square to the view");
    require(near(sfr::stereo::length(pose.up), 1) && pose.up.y > 0, "and still up");
    // An up along forward falls back to the world's.
    pose = sfr::stereo::head_pose({0, 0, 0}, {0, 0, 1}, {0, 0, 1}, 1, 0);
    require(near(pose.up, {0, 1, 0}), "degenerate up: the world's");
}
}

int main() {
    try {
        right_of_a_view();
        eye_offsets();
        halves();
        aspects();
        separations();
        head();
    } catch (const std::exception& error) {
        std::cerr << "stereo_view_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "stereo_view_test: ok\n";
    return 0;
}
