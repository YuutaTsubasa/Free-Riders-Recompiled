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

// A point `depth` straight ahead of the centre camera, seen from an eye:
// its clip-space x is the shift hud_shift gives that eye's HUD.
float clip_x(float projection_x, float separation, float depth, int eye) {
    const float x_view = -sfr::stereo::eye_offset({1, 0, 0}, separation, eye).x;
    return projection_x * x_view / depth;
}

void hud_depths() {
    using sfr::stereo::hud_shift;
    const float p = 0.9743f, sep = 0.065f;
    for (const float depth : {1.5f, 3.0f, 10.0f})
        for (int eye = 0; eye < 2; ++eye)
            require(near(hud_shift(p, sep, depth, eye), clip_x(p, sep, depth, eye), 1e-6f),
                    "the HUD moves as a point at its depth does");
    require(hud_shift(p, sep, 3, 0) > 0 && hud_shift(p, sep, 3, 1) < 0, "the left eye's HUD to the right (crossed: in front of infinity)");
    require(near(hud_shift(p, sep, 3, 0), -hud_shift(p, sep, 3, 1)), "the two eyes alike");
    require(near(hud_shift(p, sep, 1.5f, 0), 2 * hud_shift(p, sep, 3, 0)), "twice as near, twice the shift");
    require(hud_shift(p, 0, 3, 0) == 0 && hud_shift(p, sep, 0, 0) == 0, "no separation or depth: no shift");
    using sfr::stereo::default_hud_depth;
    require(near(sfr::stereo::parse_hud_depth(nullptr), default_hud_depth), "absent: the default");
    require(near(sfr::stereo::parse_hud_depth("0"), 0), "0: at infinity");
    require(near(sfr::stereo::parse_hud_depth("1.5"), 1.5f), "a number");
    require(near(sfr::stereo::parse_hud_depth("-2"), 0), "negative: 0");
    require(near(sfr::stereo::parse_hud_depth("far"), default_hud_depth), "not a number: the default");
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
        hud_depths();
        separations();
        head();
    } catch (const std::exception& error) {
        std::cerr << "stereo_view_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "stereo_view_test: ok\n";
    return 0;
}
