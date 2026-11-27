#pragma once
// The pure part of the race's stereo test view (Issue #460, race_stereo.cpp):
// where each eye's camera stands and which half of the race's view it draws
// into. No guest memory here, so tests/stereo_view_test.cpp checks it alone.
//
// The title's scene camera (vtable 821AA688/821AA8C0) keeps its view matrix
// at +32 as 16 floats, row-major for row vectors (v_view = v_world * M, as
// D3D's), built by its vt+40 from the eye (+432), target (+448) and up (+464)
// points; a projection scales the view's x by a positive number, so the
// view's x axis is the screen's right in either handedness. That axis, in
// world space, is the matrix's first column.
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace sfr::stereo {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};
constexpr Vec3 operator+(Vec3 a, Vec3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
constexpr Vec3 operator-(Vec3 a, Vec3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
constexpr Vec3 operator*(Vec3 a, float s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
constexpr float dot(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr Vec3 cross(Vec3 a, Vec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(Vec3 a) noexcept { return std::sqrt(dot(a, a)); }
// a / |a|, or {0,0,0} for a vector too short to have a direction.
inline Vec3 normalized(Vec3 a) noexcept {
    const float n = length(a);
    return n > 1e-6f && std::isfinite(n) ? a * (1.0f / n) : Vec3{};
}

// The screen's right in world space, from a row-major view matrix (see above):
// its first column, normalised.
inline Vec3 view_right(const float view[16]) noexcept { return normalized({view[0], view[4], view[8]}); }

// The two eyes: 0 the left one (drawn into the left half), 1 the right one.
inline constexpr int eye_count = 2;

// How far an eye stands from the centre camera along `right`: half the
// separation to its side (the left eye to -right).
constexpr Vec3 eye_offset(Vec3 right, float separation, int eye) noexcept {
    return right * ((eye == 0 ? -0.5f : 0.5f) * separation);
}

// An eye's part of a view x..x+width wide: the left half, then the rest
// (the right one is a pixel wider when the width is odd). Whole pixels, so
// the two halves meet without a gap or an overlap.
struct Span {
    uint32_t x = 0, width = 0;
};
constexpr Span eye_span(uint32_t x, uint32_t width, int eye) noexcept {
    const uint32_t left = width / 2;
    return eye == 0 ? Span{x, left} : Span{x + left, width - left};
}

// The aspect (width over height) an eye's camera draws at: the whole view's,
// narrowed with the width. The vertical field of view stays the title's, so
// each eye sees the middle of what the whole view would, as a two-player
// half does.
constexpr float eye_aspect(float aspect, uint32_t whole_width, uint32_t eye_width) noexcept {
    return whole_width ? aspect * float(eye_width) / float(whole_width) : aspect;
}

// SFR_TEST_STEREO_SEPARATION: the eyes' distance in the title's world units
// (metres: a rider is about 1.7 tall). Fixed choices on the Test options page;
// anything else falls back to the default, negative to 0.
inline constexpr float default_separation = 0.065f;
inline float parse_separation(const char* text) noexcept {
    if (!text || !*text) return default_separation;
    char* end = nullptr;
    const float value = std::strtof(text, &end);
    if (end == text || *end || !std::isfinite(value)) return default_separation;
    return value < 0 ? 0.0f : (value > 10.0f ? 10.0f : value);
}

// The first-person camera (SFR_TEST_FIRST_PERSON): at the rider's head,
// looking where the board is heading. `position` is the racer's (+0x120);
// `forward` and `up` its heading and up. The eye is `height` above the
// racer and `ahead` in front (out of the rider's own head); the target is a
// unit ahead of the eye.
struct Pose {
    Vec3 eye, target, up;
};
inline Pose head_pose(Vec3 position, Vec3 forward, Vec3 up, float height, float ahead) noexcept {
    const Vec3 f = normalized(forward);
    // Up made square to forward, so the view does not shear when the board
    // pitches; the world's up when the racer's is missing or along forward.
    Vec3 u = normalized(up - f * dot(up, f));
    if (length(u) == 0.0f) u = normalized(Vec3{0, 1, 0} - f * f.y);
    const Vec3 eye = position + u * height + f * ahead;
    return {eye, eye + f, u};
}

}  // namespace sfr::stereo
