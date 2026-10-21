#include "kinect_record_raw.h"
#include "kinect_v2_depth.h"
#include "kinect_v2_image.h"
#include "kinect_v2_resynth.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool close_to(float a, float b, float tolerance = 1e-3f) { return std::fabs(a - b) <= tolerance; }

uint16_t depth_at(const sfr::KinectImage& image, uint32_t x, uint32_t y) {
    const uint8_t* p = &image.pixels[(size_t(y) * image.width + x) * 2];
    return uint16_t(p[0] | p[1] << 8);
}

// Camera-space points, three floats each.
struct Points {
    std::vector<float> xyz;
    std::vector<uint8_t> body;
    void add(float x, float y, float z, uint8_t index = sfr::kinect_v2_no_body) {
        xyz.insert(xyz.end(), {x, y, z});
        body.push_back(index);
    }
    // A point that lands on pixel (column, row) of the virtual v1 at depth z.
    void at_pixel(float column, float row, float z, uint8_t index = sfr::kinect_v2_no_body) {
        add((column - 160.0f) * z / sfr::kinect_v1_depth_focal, -(row - 120.0f) * z / sfr::kinect_v1_depth_focal, z,
            index);
    }
    sfr::KinectDepthStats make(sfr::KinectImage& out, const sfr::KinectDepthView& view = {}) const {
        return sfr::kinect_v2_synthesize_depth(xyz.data(), body.data(), body.size(), view, out);
    }
};

void packing_is_the_v1s() {
    // NuiImageCamera.h: millimetres << NUI_IMAGE_PLAYER_INDEX_SHIFT (3), the
    // player in NUI_IMAGE_PLAYER_INDEX_MASK.
    require(sfr::kinect_v1_depth_pixel(2500, 1) == uint16_t(2500 << 3 | 1), "depth and player packed");
    require(sfr::kinect_v1_depth_pixel(2500, 0) == uint16_t(2500 << 3), "no player");
    require(sfr::kinect_v1_depth_pixel(800, 6) == uint16_t(800 << 3 | 6), "nearest a v1 measures");
    require(sfr::kinect_v1_depth_pixel(4000, 2) == uint16_t(4000 << 3 | 2), "farthest a v1 measures");
    // Without DISTINCT_OVERFLOW_DEPTH_VALUES too near and too far are no value.
    require(sfr::kinect_v1_depth_pixel(799, 1) == sfr::kinect_v1_depth_no_value, "too near is no value");
    require(sfr::kinect_v1_depth_pixel(4001, 1) == sfr::kinect_v1_depth_no_value, "too far is no value");
    require(sfr::kinect_v1_depth_pixel(0, 0) == 0, "unknown is no value");
    require(sfr::kinect_v1_depth_no_value == 0, "NUI_IMAGE_DEPTH_NO_VALUE");
}

void projection_is_the_nominal_lens() {
    float x = 0, y = 0;
    require(sfr::kinect_v1_depth_project({0, 0, 2}, x, y) && close_to(x, 160) && close_to(y, 120), "the axis is the centre");
    // NuiTransformSkeletonToDepthImage: 160 + X * 285.63 / Z, 120 - Y * 285.63 / Z.
    require(sfr::kinect_v1_depth_project({0.5f, 0.25f, 2.5f}, x, y), "a point in front");
    require(close_to(x, 160 + 0.5f * 285.63f / 2.5f) && close_to(y, 120 - 0.25f * 285.63f / 2.5f), "nominal focal length");
    require(!sfr::kinect_v1_depth_project({0, 0, 0}, x, y) && !sfr::kinect_v1_depth_project({0, 0, -1}, x, y),
            "nothing at or behind the camera");
}

void points_land_where_projected() {
    Points points;
    points.at_pixel(100, 50, 2.0f, 0);
    points.at_pixel(300, 200, 3.5f, sfr::kinect_v2_no_body);
    sfr::KinectImage image;
    const sfr::KinectDepthStats stats = points.make(image);
    require(image.width == 320 && image.height == 240 && image.bytes_per_pixel == 2 && image.pixels.size() == 320 * 240 * 2,
            "a v1's 320x240, two bytes a pixel");
    require(depth_at(image, 100, 50) == uint16_t(2000 << 3 | 1), "body 0 is player 1");
    require(depth_at(image, 300, 200) == uint16_t(3500 << 3), "no body is player 0");
    require(depth_at(image, 101, 50) == 0 && depth_at(image, 0, 0) == 0, "everything else unknown");
    require(stats.valid == 2 && stats.players == 1, "counts");
}

void nearest_wins() {
    Points points;
    points.at_pixel(40, 40, 3.0f, sfr::kinect_v2_no_body);  // the wall behind
    points.at_pixel(40, 40, 1.5f, 2);                        // a hand in front
    points.at_pixel(40, 40, 2.0f, sfr::kinect_v2_no_body);
    sfr::KinectImage image;
    points.make(image);
    require(depth_at(image, 40, 40) == uint16_t(1500 << 3 | 3), "the nearest point and its player");
    // And the other way round in arrival order.
    Points reversed;
    reversed.at_pixel(40, 40, 1.5f, 2);
    reversed.at_pixel(40, 40, 3.0f, sfr::kinect_v2_no_body);
    reversed.make(image);
    require(depth_at(image, 40, 40) == uint16_t(1500 << 3 | 3), "a farther point does not overwrite");
}

void holes_filled_conservatively() {
    Points points;
    // A one-pixel gap between two that agree: filled with their mean.
    points.at_pixel(10, 10, 2.00f, 0);
    points.at_pixel(12, 10, 2.04f, 0);
    // Between two that do not (an edge): left.
    points.at_pixel(20, 10, 2.0f, 0);
    points.at_pixel(22, 10, 3.0f, sfr::kinect_v2_no_body);
    // Between two players: left.
    points.at_pixel(30, 10, 2.0f, 0);
    points.at_pixel(32, 10, 2.0f, 1);
    // Vertically too.
    points.at_pixel(50, 20, 2.5f);
    points.at_pixel(50, 22, 2.5f);
    // A two-pixel gap: left.
    points.at_pixel(60, 10, 2.0f);
    points.at_pixel(63, 10, 2.0f);
    sfr::KinectImage image;
    const sfr::KinectDepthStats stats = points.make(image);
    require(depth_at(image, 11, 10) == uint16_t(2020 << 3 | 1), "a gap between agreeing pixels is filled");
    require(depth_at(image, 21, 10) == 0, "an edge is not filled");
    require(depth_at(image, 31, 10) == 0, "two players are not joined");
    require(depth_at(image, 50, 21) == uint16_t(2500 << 3), "vertical gap filled");
    require(depth_at(image, 61, 10) == 0 && depth_at(image, 62, 10) == 0, "wider gaps stay unknown");
    require(stats.filled == 2, "two filled");
}

void out_of_range_is_no_value() {
    Points points;
    points.at_pixel(5, 5, 0.5f, 0);   // too near for a v1
    points.at_pixel(6, 5, 4.5f, 0);   // too far
    points.add(0, 0, 0);              // v2 measured nothing
    points.add(0, 0, -INFINITY);      // the mapper's no point
    points.add(NAN, 0, 2);
    sfr::KinectImage image;
    const sfr::KinectDepthStats stats = points.make(image);
    require(depth_at(image, 5, 5) == 0 && depth_at(image, 6, 5) == 0 && depth_at(image, 160, 120) == 0,
            "outside the v1's range, or no point, is no value");
    require(stats.valid == 0 && stats.players == 0, "nothing valid");
}

void view_moves_the_camera() {
    // The virtual camera 1.5 m behind the v2: a point 1 m in front of the v2
    // is 2.5 m from it, and a point to the side comes towards the centre.
    sfr::KinectDepthView view;
    view.origin = {0, 0, -1.5f};
    const auto seen = sfr::kinect_depth_view_point(view, {0.4f, 0.1f, 1.0f});
    require(close_to(seen[0], 0.4f) && close_to(seen[1], 0.1f) && close_to(seen[2], 2.5f), "moved along z");
    const auto back = sfr::kinect_depth_view_unpoint(view, seen);
    require(close_to(back[2], 1.0f), "and back");
    Points points;
    points.add(0.4f, 0.1f, 1.0f, 0);
    sfr::KinectImage image;
    points.make(image, view);
    float x = 0, y = 0;
    sfr::kinect_v1_depth_project(seen, x, y);
    require(depth_at(image, uint32_t(std::lround(x)), uint32_t(std::lround(y))) == uint16_t(2500 << 3 | 1),
            "the image is the moved camera's");

    // The skeletons and the floor move with it.
    sfr::KinectFrame frame;
    frame.floor_plane = {0, 1, 0, 0.9f};  // the floor 0.9 m below the v2
    frame.gravity = {0, 1, 0};
    sfr::KinectBody& body = frame.bodies.emplace_back();
    body.position = {0.1f, 0.0f, 1.2f};
    body.joints[0] = {0.1f, 0.0f, 1.2f};
    body.hand_tips[0] = {};  // not seen: stays none
    view.origin = {0, 0.2f, -1.3f};
    sfr::kinect_depth_view_frame(view, frame);
    require(close_to(body.position[2], 2.5f) && close_to(body.position[1], -0.2f), "skeleton centre moved");
    require(close_to(body.joints[0][2], 2.5f), "joints moved");
    require(body.hand_tips[0][0] == 0 && body.hand_tips[0][2] == 0, "an unseen point stays none");
    // The floor at y = -0.9 in the v2 is y = -1.1 to a camera 0.2 m higher.
    require(close_to(frame.floor_plane[3], 1.1f), "floor plane moved");
    require(close_to(frame.gravity[1], 1.0f), "gravity is a direction");
}

sfr::KinectFrame frame_with(std::initializer_list<std::pair<uint32_t, std::array<float, 3>>> bodies,
                            std::array<float, 4> floor = {}) {
    sfr::KinectFrame frame;
    frame.floor_plane = floor;
    for (const auto& [id, position] : bodies) {
        sfr::KinectBody& body = frame.bodies.emplace_back();
        body.tracking_id = id;
        body.position = position;
    }
    return frame;
}

void settings_from_text() {
    const auto off = sfr::KinectDepthSettings::parse(nullptr, nullptr, nullptr);
    require(off.mode == sfr::KinectDepthViewMode::fit && !off.player_view && off.fit_scale == 0 &&
                close_to(off.distance, 2.5f) && close_to(off.height, 0.8f),
            "defaults: fit, the scale from the lens");
    require(std::string(off.view_name()) == "fit", "named fit");
    const auto set = sfr::KinectDepthSettings::parse("sensor", "3", "1.2");
    require(set.mode == sfr::KinectDepthViewMode::sensor && !set.player_view && close_to(set.distance, 3) &&
                close_to(set.height, 1.2f),
            "set");
    const auto odd = sfr::KinectDepthSettings::parse("player", "9", "x");
    require(odd.player_view && odd.mode == sfr::KinectDepthViewMode::player && close_to(odd.distance, 4) &&
                close_to(odd.height, 0.8f),
            "clamped");
    require(close_to(sfr::KinectDepthSettings::parse("fit", nullptr, nullptr, "1.5").fit_scale, 1.5f) &&
                close_to(sfr::KinectDepthSettings::parse("fit", nullptr, nullptr, "3").fit_scale, 2.0f) &&
                close_to(sfr::KinectDepthSettings::parse("fit", nullptr, nullptr, "0.5").fit_scale, 1.0f),
            "fit scale set and clamped to 1..2");
    require(sfr::KinectDepthSettings::parse("odd", nullptr, nullptr).mode == sfr::KinectDepthViewMode::fit,
            "an unknown view is fit");
}

void latch_holds_still() {
    sfr::KinectDepthSettings settings;
    settings.mode = sfr::KinectDepthViewMode::player;
    settings.player_view = true;
    sfr::KinectDepthViewLatch latch(settings);
    double t = 0;
    const auto step = [&](const sfr::KinectFrame& frame) {
        t += 1.0 / 30.0;
        return latch.update(frame, t);
    };
    // Nobody: the v2's own place.
    require(close_to(step(frame_with({})).origin[2], 0), "nobody, nothing moved");
    // Someone at 1.2 m: placed at once, latched after settling.
    sfr::KinectDepthView view = step(frame_with({{7, {0.0f, 0.0f, 1.2f}}}));
    require(close_to(view.origin[2], 1.2f - 2.5f) && !latch.latched(), "first frame places it");
    for (uint32_t i = 1; i < sfr::KinectDepthViewLatch::settle_frames; ++i) view = step(frame_with({{7, {0.0f, 0.0f, 1.2f}}}));
    require(latch.latched() && close_to(view.origin[2], -1.3f), "latched at the settled place");
    // Stepping aside, crouching, stepping forward: the camera stays.
    view = step(frame_with({{7, {0.5f, -0.3f, 1.0f}}}));
    require(close_to(view.origin[0], 0) && close_to(view.origin[1], 0) && close_to(view.origin[2], -1.3f),
            "the player's moves are not followed");
    // A second player stepping in does not move it either.
    view = step(frame_with({{7, {0.0f, 0.0f, 1.2f}}, {9, {0.0f, 0.0f, 0.8f}}}));
    require(close_to(view.origin[2], -1.3f), "a second player does not move it");
    // A short gap keeps it; a second away lets it go, and the next person
    // places it again.
    for (int i = 0; i < 10; ++i) view = step(frame_with({}));
    require(latch.latched() && close_to(view.origin[2], -1.3f), "a third of a second away keeps it");
    for (int i = 0; i < 30; ++i) step(frame_with({}));
    require(!latch.latched(), "a second away lets it go");
    view = step(frame_with({{11, {0.3f, 0.0f, 2.0f}}, {12, {-0.1f, 0.0f, 1.6f}}}));
    require(close_to(view.origin[2], 1.6f - 2.5f), "the most central body places it again");
}

void latch_settles_on_the_mean() {
    sfr::KinectDepthSettings settings;
    settings.mode = sfr::KinectDepthViewMode::player;
    settings.player_view = true;
    sfr::KinectDepthViewLatch latch(settings);
    double t = 0;
    sfr::KinectDepthView view;
    // Walking in: 1.0 m to about 1.28 m over the settling frames.
    double sum = 0;
    for (uint32_t i = 0; i < sfr::KinectDepthViewLatch::settle_frames; ++i) {
        const float z = 1.0f + 0.02f * float(i);
        sum += z;
        view = latch.update(frame_with({{3, {0, 0, z}}}), t += 1.0 / 30.0);
    }
    require(latch.latched() && close_to(view.origin[2], float(sum / sfr::KinectDepthViewLatch::settle_frames) - 2.5f),
            "the mean of the settling frames");
}

void latch_height_from_the_floor() {
    sfr::KinectDepthSettings settings;
    settings.mode = sfr::KinectDepthViewMode::player;
    settings.player_view = true;
    settings.height = 0.8f;
    const sfr::KinectDepthViewLatch latch(settings);
    // The v2 1.4 m above the floor (on top of a TV), level; the hips 0.95 m
    // above the floor at y = -0.45, 1.2 m away.
    const auto view = latch.place({0.2f, -0.45f, 1.2f}, {0, 1, 0, 1.4f});
    require(close_to(view.origin[2], -1.3f), "distance");
    // The reference sits 0.15 m above a camera 0.8 m over the floor: the
    // camera comes down 0.6 m.
    require(close_to(view.origin[1], -0.6f), "height above the floor");
    require(close_to(view.origin[0], 0), "never sideways");
    const auto seen = sfr::kinect_depth_view_point(view, {0.2f, -0.45f, 1.2f});
    require(close_to(seen[1], 0.15f) && close_to(seen[2], 2.5f), "the reference where a v1 would see it");
    // Without a floor, the height stays the v2's.
    require(close_to(latch.place({0.2f, -0.45f, 1.2f}, {}).origin[1], 0), "no floor, no height change");
}

sfr::KinectDepthView fit_view(float k) {
    sfr::KinectDepthView view;
    view.z_scale = k;
    return view;
}

void fit_scale_from_the_lens() {
    // 285.63 / (366 * 240 / 424) = 1.3788.
    require(close_to(sfr::kinect_v2_fit_scale(366.0f), 285.63f / (366.0f * 240.0f / 424.0f)) &&
                close_to(sfr::kinect_v2_fit_scale(366.0f), 1.379f, 1e-3f),
            "the typical v2's stretch");
    require(close_to(sfr::kinect_v2_fit_scale(0), sfr::kinect_v2_fit_scale(366.0f)), "no lens: the typical one");
    const float k = sfr::kinect_v2_fit_scale(366.0f);
    require(close_to(sfr::kinect_depth_true_minimum(k), 0.8f / k) && close_to(sfr::kinect_depth_true_maximum(k), 4.0f / k),
            "the true range");
    require(close_to(sfr::kinect_depth_true_minimum(k), 0.58f, 0.01f) && close_to(sfr::kinect_depth_true_maximum(k), 2.90f, 0.01f),
            "about 0.58 to 2.90 m");
}

void fit_shows_the_whole_v2_view() {
    const float k = sfr::kinect_v2_fit_scale(366.0f);
    const auto view = fit_view(k);
    // The v2's corner rays (pixel (0, 0) and (511, 423) through its typical
    // lens) land inside the v1's 320x240; at sensor they do not.
    for (const auto& corner : {std::array<float, 2>{0, 0}, std::array<float, 2>{511, 423}, std::array<float, 2>{0, 423}}) {
        const float z = 2.0f;
        const std::array<float, 3> p{(corner[0] - 256.0f) * z / 366.0f, -(corner[1] - 212.0f) * z / 366.0f, z};
        float x = 0, y = 0;
        require(sfr::kinect_v1_depth_project(sfr::kinect_depth_view_point(view, p), x, y), "projected");
        require(x >= -0.5f && x < 320.5f && y >= -0.5f && y < 240.5f, "a corner of the v2's view inside the v1's image");
        sfr::kinect_v1_depth_project(p, x, y);
        require(x < 0 || x >= 320 || y < 0 || y >= 240, "outside without the stretch");
    }
    // The top and bottom rows of the v2 fill the v1's rows.
    float x = 0, y = 0;
    sfr::kinect_v1_depth_project(sfr::kinect_depth_view_point(view, {0, 212.0f * 2.0f / 366.0f, 2.0f}), x, y);
    require(close_to(y, 0.0f, 0.01f), "the v2's top row is the v1's");
}

void fit_depth_matches_size() {
    const float k = 1.4f;
    // A person 1.8 m tall at 1.5 m through the fit view looks as tall as
    // one at 2.1 m through the v1, and its depth is 2.1 m.
    const auto view = fit_view(k);
    float x0 = 0, top = 0, bottom = 0;
    sfr::kinect_v1_depth_project(sfr::kinect_depth_view_point(view, {0, 0.9f, 1.5f}), x0, top);
    sfr::kinect_v1_depth_project(sfr::kinect_depth_view_point(view, {0, -0.9f, 1.5f}), x0, bottom);
    float v1_top = 0, v1_bottom = 0;
    sfr::kinect_v1_depth_project({0, 0.9f, 2.1f}, x0, v1_top);
    sfr::kinect_v1_depth_project({0, -0.9f, 2.1f}, x0, v1_bottom);
    require(close_to(bottom - top, v1_bottom - v1_top), "the same size as at k times the distance");
    Points points;
    points.at_pixel(0, 0, 1.0f);  // replaced below
    points.xyz = {0.0f, 0.0f, 1.5f};
    points.body = {0};
    sfr::KinectImage image;
    points.make(image, view);
    require(depth_at(image, 160, 120) == uint16_t(2100 << 3 | 1), "with that depth");
}

void fit_range_clamps() {
    const auto view = fit_view(1.4f);
    Points points;
    points.add(0.0f, 0.0f, 0.55f, 0);   // 0.77 m stretched: too near
    points.add(-0.1f, 0.0f, 0.60f, 0);  // 0.84 m: in
    points.add(0.2f, 0.0f, 2.80f, 0);   // 3.92 m: in
    points.add(0.4f, 0.0f, 2.90f, 0);   // 4.06 m: too far
    sfr::KinectImage image;
    const sfr::KinectDepthStats stats = points.make(image, view);
    require(stats.valid == 2, "only the stretched 800..4000 mm");
    float x = 0, y = 0;
    sfr::kinect_v1_depth_project(sfr::kinect_depth_view_point(view, {0.0f, 0.0f, 0.55f}), x, y);
    require(depth_at(image, uint32_t(std::lround(x)), uint32_t(std::lround(y))) == 0, "too near is no value");
}

void fit_moves_skeleton_and_floor() {
    const float k = 1.4f;
    const auto view = fit_view(k);
    sfr::KinectFrame frame;
    // A floor 1 m below a v2 tilted down a little: n = (0, cos, sin).
    const float c = std::cos(0.2f), s = std::sin(0.2f);
    frame.floor_plane = {0, c, s, 1.0f};
    frame.gravity = {0, c, s};
    sfr::KinectBody& body = frame.bodies.emplace_back();
    body.position = {0.1f, -0.2f, 1.5f};
    body.joints[3] = {0.0f, 0.5f, 1.4f};
    body.hand_tips[1] = {0.3f, 0.1f, 1.2f};
    // A point on the floor before is on it after.
    const std::array<float, 3> on_floor{0.4f, -(1.0f + s * 2.0f) / c, 2.0f};
    sfr::kinect_depth_view_frame(view, frame);
    require(close_to(body.position[2], 1.5f * k) && close_to(body.position[0], 0.1f) && close_to(body.position[1], -0.2f),
            "the skeleton's centre stretched");
    require(close_to(body.joints[3][2], 1.4f * k) && close_to(body.hand_tips[1][2], 1.2f * k), "joints and tips");
    require(body.joints[0] == std::array<float, 3>{} && body.thumbs[0] == std::array<float, 3>{}, "none stays none");
    const auto& n = frame.floor_plane;
    require(close_to(std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]), 1.0f), "a unit normal");
    const auto seen = sfr::kinect_depth_view_point(view, on_floor);
    require(close_to(n[0] * seen[0] + n[1] * seen[1] + n[2] * seen[2] + n[3], 0.0f), "the floor goes through the same space");
    require(close_to(frame.gravity[1], n[1]) && close_to(frame.gravity[2], n[2]), "gravity the new floor normal");
    // And back: what the preview does with a handed-out point.
    const auto back = sfr::kinect_depth_view_unpoint(view, body.joints[3]);
    require(close_to(back[2], 1.4f) && close_to(back[1], 0.5f), "undone");
}

void fit_of_one_is_sensor() {
    Points points;
    points.add(0.3f, -0.2f, 2.0f, 1);
    points.add(-0.5f, 0.4f, 3.1f, sfr::kinect_v2_no_body);
    sfr::KinectImage fit, sensor;
    points.make(fit, fit_view(1.0f));
    points.make(sensor, {});
    require(fit.pixels == sensor.pixels, "a stretch of one is the sensor view");
    sfr::KinectFrame a, b;
    a.floor_plane = b.floor_plane = {0, 1, 0, 0.8f};
    a.gravity = b.gravity = {0, 1, 0};
    a.bodies.emplace_back().position = {0.1f, 0.2f, 1.9f};
    b.bodies = a.bodies;
    sfr::kinect_depth_view_frame(fit_view(1.0f), a);
    require(a.floor_plane == b.floor_plane && a.bodies[0].position == b.bodies[0].position, "nothing moved");
}

void sensor_view_stays() {    sfr::KinectDepthSettings settings;
    settings.player_view = false;
    sfr::KinectDepthViewLatch latch(settings);
    for (int i = 0; i < 30; ++i) {
        const auto& view = latch.update(frame_with({{1, {0.0f, 0.0f, 1.0f}}}, {0, 1, 0, 1}), i / 30.0);
        require(view.origin == std::array<float, 3>{}, "the sensor view is the v2's own place");
    }
}

void lens_fallback_matches_estimate() {
    // A typical v2 lens: the inverse of kinect_v2_depth_estimate.
    std::vector<uint16_t> depth(sfr::kinect_v2_depth_width * sfr::kinect_v2_depth_height, 0);
    depth[100 * sfr::kinect_v2_depth_width + 300] = 2000;
    std::vector<float> points;
    sfr::kinect_v2_depth_points_from_lens(depth.data(), sfr::kinect_v2_depth_width, sfr::kinect_v2_depth_height, points);
    const float* p = &points[3 * (100 * sfr::kinect_v2_depth_width + 300)];
    require(close_to(p[2], 2.0f) && close_to(p[0], (300 - 256) * 2.0f / 366.0f) && close_to(p[1], -(100 - 212) * 2.0f / 366.0f),
            "unprojected through the typical lens");
    require(points[2] == 0, "no depth, no point");
}

// A floor below a v2 that looks down by pitch and is rolled by roll
// (radians), d metres above it: its normal in the v2's space.
std::array<float, 4> tilted_floor(float pitch, float roll, float d) {
    // Up in a level camera is (0, 1, 0); the camera pitched down sees it
    // leaning back towards itself (-z), rolled to its +x.
    const float nx = std::sin(roll) * std::cos(pitch), ny = std::cos(roll) * std::cos(pitch), nz = -std::sin(pitch);
    return {nx, ny, nz, d};
}

std::array<float, 3> times(const std::array<float, 9>& r, const std::array<float, 3>& p) {
    return {r[0] * p[0] + r[1] * p[1] + r[2] * p[2], r[3] * p[0] + r[4] * p[1] + r[5] * p[2],
            r[6] * p[0] + r[7] * p[1] + r[8] * p[2]};
}

void levelling_takes_out_a_known_tilt() {
    const float pitch = 15.0f * 3.14159265f / 180.0f, roll = 4.0f * 3.14159265f / 180.0f;
    const auto floor = tilted_floor(pitch, roll, 1.3f);
    require(sfr::kinect_floor_known(floor), "a tilted floor is a floor");
    const auto r = sfr::kinect_level_rotation(floor);
    const auto up = times(r, {floor[0], floor[1], floor[2]});
    require(close_to(up[0], 0, 1e-5f) && close_to(up[1], 1, 1e-5f) && close_to(up[2], 0, 1e-5f), "the normal turned straight up");
    // A rotation: rows of unit length, at right angles.
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const float dot = r[3 * i] * r[3 * j] + r[3 * i + 1] * r[3 * j + 1] + r[3 * i + 2] * r[3 * j + 2];
            require(close_to(dot, i == j ? 1.0f : 0.0f, 1e-5f), "orthonormal");
        }
    // No yaw: the optical axis keeps its heading (no sideways part).
    const auto forward = times(r, {0, 0, 1});
    require(close_to(forward[0], 0, 1e-6f) && forward[2] > 0.9f, "the heading stays");
    // A point on the floor is d below the levelled camera.
    const std::array<float, 3> on_floor{0.7f, 0, 2.4f};
    std::array<float, 3> p = on_floor;
    // y from n.p + d = 0.
    p[1] = -(floor[3] + floor[0] * p[0] + floor[2] * p[2]) / floor[1];
    require(close_to(times(r, p)[1], -1.3f, 1e-4f), "the floor is level, d below");
    float tilt_pitch = 0, tilt_roll = 0;
    sfr::kinect_floor_tilt(floor, tilt_pitch, tilt_roll);
    require(close_to(tilt_pitch, 15.0f, 0.01f) && close_to(tilt_roll, 4.0f, 0.2f), "the tilt for the log");
    // No floor: no turn.
    const auto identity = sfr::kinect_level_rotation({0, 0, 0, 0});
    require(identity == std::array<float, 9>{1, 0, 0, 0, 1, 0, 0, 0, 1} && !sfr::kinect_floor_known({0, 0, 0, 0}) &&
                !sfr::kinect_floor_known({0, -1, 0, 1}),
            "an unknown floor is not levelled");
}

sfr::KinectDepthSettings ideal_settings() {
    return sfr::KinectDepthSettings::parse("ideal", nullptr, nullptr);
}

void ideal_settings_from_text() {
    const auto s = ideal_settings();
    require(s.mode == sfr::KinectDepthViewMode::ideal && std::string(s.view_name()) == "ideal" && !s.player_view,
            "named ideal");
    require(close_to(s.distance, 2.5f) && close_to(s.height, 1.0f) && close_to(s.follow, 2.0f) && close_to(s.deadband, 0.15f),
            "defaults: 2.5 m, 1.0 m up, 2 s, 0.15 m");
    const auto set = sfr::KinectDepthSettings::parse("ideal", "3", "0.6", nullptr, "50", "-1");
    require(close_to(set.distance, 3) && close_to(set.height, 0.6f) && close_to(set.follow, 20) && close_to(set.deadband, 0),
            "set and clamped");
    require(close_to(sfr::KinectDepthSettings::parse("player", nullptr, nullptr).height, 0.8f), "the player view keeps 0.8 m");
}

// Steps an ideal state at 30 frames a second.
struct IdealRun {
    sfr::KinectDepthSettings settings = ideal_settings();
    sfr::KinectIdealState state;
    double t = 0;
    const sfr::KinectIdealState& step(const sfr::KinectFrame& frame, int frames = 1) {
        for (int i = 0; i < frames; ++i) {
            t += 1.0 / 30.0;
            state = sfr::kinect_ideal_step(state, settings, t, frame);
        }
        return state;
    }
    sfr::KinectDepthView view() const { return sfr::kinect_ideal_view(state, settings); }
};

void ideal_follows_slowly() {
    const std::array<float, 4> level{0, 1, 0, 1.0f};  // the v2 1 m above the floor, level
    IdealRun run;
    // Nobody yet: in front of the v2's own place, at the height.
    run.step(frame_with({}, level));
    require(!run.state.placed && close_to(run.view().origin[2], 0) && close_to(run.view().origin[1], 0), "nobody yet");
    // The first body is taken at once.
    run.step(frame_with({{1, {0.3f, -0.2f, 2.0f}}}, level));
    require(run.state.placed && close_to(run.state.reference[0], 0.3f) && close_to(run.state.reference[1], 2.0f),
            "the first body snaps");
    auto view = run.view();
    require(close_to(view.origin[0], 0.3f) && close_to(view.origin[1], 0.0f) && close_to(view.origin[2], -0.5f),
            "the camera 2.5 m in front, 1 m above the floor");
    // Leaning and small steps inside the dead band: the camera stays.
    run.step(frame_with({{1, {0.42f, -0.2f, 1.9f}}}, level), 60);
    require(close_to(run.state.reference[0], 0.3f, 1e-6f) && close_to(run.state.reference[1], 2.0f, 1e-6f),
            "inside the dead band nothing moves");
    // A step of 1 m held: only what is beyond the band, with the time constant.
    run.step(frame_with({{1, {1.3f, -0.2f, 2.0f}}}, level), 60);  // two seconds, one time constant
    const float expected = 1.3f - 0.15f - 0.85f * std::exp(-1.0f);
    require(close_to(run.state.reference[0], expected, 2e-3f), "glides: one time constant, 63% of the way");
    run.step(frame_with({{1, {1.3f, -0.2f, 2.0f}}}, level), 600);
    require(close_to(run.state.reference[0], 1.15f, 1e-3f), "and settles at the edge of the band");
    // Height never follows the player.
    run.step(frame_with({{1, {1.3f, 0.4f, 2.0f}}}, level), 30);
    require(close_to(run.view().origin[1], 0.0f), "a hop does not lift the camera");
    // Nobody: held. The next body, wherever, snaps.
    const auto held = run.state.reference;
    run.step(frame_with({}, level), 90);
    require(run.state.reference == held && !run.state.had_bodies, "nobody holds the camera");
    run.step(frame_with({{4, {-0.6f, -0.2f, 3.0f}}}, level));
    require(close_to(run.state.reference[0], -0.6f) && close_to(run.state.reference[1], 3.0f), "the next body snaps");
}

void ideal_two_players() {
    const std::array<float, 4> level{0, 1, 0, 1.0f};
    IdealRun run;
    // Two players: their mean.
    run.step(frame_with({{1, {-0.5f, 0, 2.2f}}, {2, {0.5f, 0, 2.6f}}}, level));
    require(close_to(run.state.reference[0], 0.0f) && close_to(run.state.reference[1], 2.4f) && run.state.bodies == 2,
            "two players: the mean");
    // One leaves: glides (no jump) towards the one left.
    run.step(frame_with({{2, {0.5f, 0, 2.6f}}}, level));
    require(run.state.reference[0] > 0.0f && run.state.reference[0] < 0.05f, "a player leaving glides, not jumps");
    run.step(frame_with({{2, {0.5f, 0, 2.6f}}}, level), 900);
    require(close_to(run.state.reference[0], 0.35f, 1e-3f) && close_to(run.state.reference[1], 2.45f, 1e-3f),
            "towards the one left, up to the band");
}

void ideal_is_deterministic() {
    const auto floor = tilted_floor(0.2f, 0.03f, 1.4f);
    const auto play = [&] {
        IdealRun run;
        std::vector<sfr::KinectDepthView> views;
        for (int i = 0; i < 300; ++i) {
            const float x = 0.6f * std::sin(i * 0.05f), z = 2.0f + 0.3f * std::cos(i * 0.03f);
            if (i % 70 < 60)
                run.step(frame_with({{1, {x, -0.3f, z}}, {2, {x + 0.8f, -0.3f, z + 0.2f}}}, i < 20 ? std::array<float, 4>{} : floor));
            else
                run.step(frame_with({}, floor));
            views.push_back(run.view());
        }
        return views;
    };
    const auto a = play(), b = play();
    require(a.size() == b.size(), "same length");
    for (size_t i = 0; i < a.size(); ++i)
        require(std::memcmp(&a[i].origin, &b[i].origin, sizeof a[i].origin) == 0 &&
                    std::memcmp(&a[i].rotation, &b[i].rotation, sizeof a[i].rotation) == 0 && a[i].rotated == b[i].rotated,
                "the same inputs give the same camera, bit for bit");
}

void ideal_without_a_floor() {
    IdealRun run;
    run.step(frame_with({{1, {0.2f, -0.1f, 1.8f}}}));
    auto view = run.view();
    require(!view.rotated && !run.state.floor_known && close_to(view.origin[1], 0) && close_to(view.origin[2], 1.8f - 2.5f),
            "no floor: no turn, the sensor's own height");
    // The floor turns up (a tilt): levelled from now, the reference taken again.
    const auto floor = tilted_floor(0.25f, 0.0f, 1.5f);
    run.step(frame_with({{1, {0.2f, -0.1f, 1.8f}}}, floor));
    view = run.view();
    const auto r = sfr::kinect_level_rotation(floor);
    const auto expected = times(r, {0.2f, -0.1f, 1.8f});
    require(view.rotated && close_to(run.state.reference[0], expected[0]) && close_to(run.state.reference[1], expected[2]),
            "levelled and placed again at once");
    require(close_to(view.origin[1], -1.5f + 1.0f), "the camera 1 m above the floor");
    // A frame without a floor keeps the one found.
    run.step(frame_with({{1, {0.2f, -0.1f, 1.8f}}}));
    require(run.state.floor_known && close_to(run.state.floor[3], 1.5f), "a lost floor keeps the last");
}

void ideal_is_a_true_v1() {
    // The v2 on top of a TV, 1.5 m above the floor, looking down 12 degrees;
    // the player 2.0 m away. The ideal camera stands 2.5 m in front of the
    // player, 1.0 m above the floor, level: what it sees is what a real v1
    // there sees, not stretched.
    const float pitch = 12.0f * 3.14159265f / 180.0f;
    const auto floor = tilted_floor(pitch, 0.0f, 1.5f);
    const auto r = sfr::kinect_level_rotation(floor);
    // The world (levelled v2 space) to the v2's own: the transpose.
    const auto to_sensor = [&](const std::array<float, 3>& w) {
        return std::array<float, 3>{r[0] * w[0] + r[3] * w[1] + r[6] * w[2], r[1] * w[0] + r[4] * w[1] + r[7] * w[2],
                                    r[2] * w[0] + r[5] * w[1] + r[8] * w[2]};
    };
    // The player's hips 0.95 m above the floor, 2.0 m ahead, 0.1 m aside.
    const std::array<float, 3> hips_world{0.1f, -1.5f + 0.95f, 2.0f};
    IdealRun run;
    run.step(frame_with({{1, to_sensor(hips_world)}}, floor));
    const auto view = run.view();
    // A real v1 at (0.1, -0.5, -0.5) in the world, level.
    const std::array<float, 3> v1{0.1f, -1.5f + 1.0f, 2.0f - 2.5f};
    require(close_to(view.origin[0], v1[0]) && close_to(view.origin[1], v1[1]) && close_to(view.origin[2], v1[2]),
            "the camera where a v1 would stand");
    // A head-height point of the player at true distance 2.0: depth 2.5 m from
    // the v1, and on the pixel the v1's lens puts it.
    const std::array<float, 3> head_world{0.25f, -1.5f + 1.6f, 2.0f};
    const std::array<float, 3> seen{head_world[0] - v1[0], head_world[1] - v1[1], head_world[2] - v1[2]};
    Points points;
    const auto p = to_sensor(head_world);
    points.add(p[0], p[1], p[2], 0);
    sfr::KinectImage image;
    points.make(image, view);
    float x = 0, y = 0;
    sfr::kinect_v1_depth_project(seen, x, y);
    require(close_to(seen[2], 2.5f), "2.5 m from the virtual v1");
    require(depth_at(image, uint32_t(std::lround(x)), uint32_t(std::lround(y))) == uint16_t(2500 << 3 | 1),
            "the v1's pixel, the true depth from it (no stretch)");
    // A 1.8 m person at 2.5 m from it: as tall as through a v1 (285.63 * 1.8 / 2.5).
    float x0 = 0, top = 0, bottom = 0;
    sfr::kinect_v1_depth_project(sfr::kinect_depth_view_point(view, to_sensor({0.1f, -1.5f + 1.8f, 2.0f})), x0, top);
    sfr::kinect_v1_depth_project(sfr::kinect_depth_view_point(view, to_sensor({0.1f, -1.5f, 2.0f})), x0, bottom);
    require(close_to(bottom - top, 285.63f * 1.8f / 2.5f, 0.05f), "a v1's size at the distance");
}

void ideal_moves_skeleton_and_floor() {
    const auto floor = tilted_floor(0.2f, -0.05f, 1.2f);
    IdealRun run;
    sfr::KinectFrame frame = frame_with({{1, {0.1f, -0.2f, 1.7f}}}, floor);
    frame.gravity = {floor[0], floor[1], floor[2]};
    run.step(frame);
    const auto view = run.view();
    sfr::KinectBody& body = frame.bodies[0];
    body.joints[3] = {0.0f, 0.5f, 1.6f};  // the head
    body.hand_tips[1] = {0.3f, 0.1f, 1.2f};
    const std::array<float, 3> head = body.joints[3];
    sfr::kinect_depth_view_frame(view, frame);
    // The joint lands on the depth image where the same point of the scene does.
    Points points;
    points.add(head[0], head[1], head[2], 0);
    sfr::KinectImage image;
    points.make(image, view);
    float x = 0, y = 0;
    require(sfr::kinect_v1_depth_project(body.joints[3], x, y), "the head projects");
    const uint16_t value = depth_at(image, uint32_t(std::lround(x)), uint32_t(std::lround(y)));
    require((value & 7) == 1 && close_to(float(value >> 3) / 1000.0f, body.joints[3][2], 0.001f),
            "skeleton and depth agree");
    require(body.thumbs[0] == std::array<float, 3>{} && body.joints[0] == std::array<float, 3>{}, "none stays none");
    // The floor: level, the camera height below it.
    const auto& n = frame.floor_plane;
    require(close_to(n[0], 0, 1e-4f) && close_to(n[1], 1, 1e-4f) && close_to(n[2], 0, 1e-4f) && close_to(n[3], 1.0f, 1e-4f),
            "the floor level, 1 m below the camera");
    require(close_to(frame.gravity[1], 1.0f, 1e-4f) && close_to(frame.gravity[2], 0, 1e-4f), "gravity straight up");
    // And back, as the preview does.
    const auto back = sfr::kinect_depth_view_unpoint(view, body.joints[3]);
    require(close_to(back[0], head[0], 1e-5f) && close_to(back[1], head[1], 1e-5f) && close_to(back[2], head[2], 1e-5f),
            "the preview's way back");
}

void viewer_keeps_the_old_views() {
    // fit, sensor and player through the viewer are what they were.
    sfr::KinectDepthSettings fit = sfr::KinectDepthSettings::parse("fit", nullptr, nullptr);
    sfr::KinectDepthViewer fit_viewer(fit);
    fit_viewer.set_fit_scale(1.25f);
    require(fit_viewer.body_frame(frame_with({{1, {0, 0, 1}}}), 1.0).z_scale == 1.25f &&
                fit_viewer.depth_frame(1.0).z_scale == 1.25f && fit_viewer.view().origin == std::array<float, 3>{},
            "fit");
    sfr::KinectDepthViewer sensor(sfr::KinectDepthSettings::parse("sensor", nullptr, nullptr));
    require(sensor.body_frame(frame_with({{1, {0, 0, 1}}}), 1.0).origin == std::array<float, 3>{} &&
                sensor.depth_frame(1.1).z_scale == 1.0f && !sensor.depth_frame(1.1).rotated,
            "sensor");
    const auto player_settings = sfr::KinectDepthSettings::parse("player", nullptr, nullptr);
    sfr::KinectDepthViewer player(player_settings);
    sfr::KinectDepthViewLatch latch(player_settings);
    for (int i = 0; i < 40; ++i) {
        const auto frame = frame_with({{7, {0.01f * i, 0.0f, 1.2f + 0.01f * i}}}, {0, 1, 0, 1.4f});
        const auto& a = player.body_frame(frame, i / 30.0);
        const auto& b = latch.update(frame, i / 30.0);
        require(a.origin == b.origin && player.depth_frame(i / 30.0).origin == b.origin, "player: the latch's view");
    }
}

bool same_view(const sfr::KinectDepthView& a, const sfr::KinectDepthView& b) {
    return a.origin == b.origin && a.z_scale == b.z_scale && a.rotated == b.rotated && a.rotation == b.rotation;
}

void skeleton_knobs() {
    // Unset: sensor, player and ideal hand the skeleton over through the
    // depth's view; fit hands it over in real metres while its depth is
    // stretched by k (the title never projects the skeleton into the depth).
    const auto frame = frame_with({{1, {0.1f, -0.2f, 1.9f}}}, tilted_floor(0.2f, 0.0f, 1.2f));
    for (const char* name : {"fit", "sensor", "player", "ideal"}) {
        sfr::KinectDepthViewer viewer(sfr::KinectDepthSettings::parse(name, nullptr, nullptr));
        viewer.set_fit_scale(1.379f);
        viewer.body_frame(frame, 1.0);
        viewer.depth_frame(1.0);
        if (std::string_view(name) == "fit") {
            require(close_to(viewer.view().z_scale, 1.379f) && close_to(viewer.skeleton_view().z_scale, 1.0f) &&
                        viewer.skeleton_view().origin == viewer.view().origin,
                    "unset, fit's skeleton is in real metres and only its depth is stretched");
        } else {
            require(same_view(viewer.skeleton_view(), viewer.view()), "unset knobs change nothing for the other views");
        }
    }
    {
        const auto settings = sfr::KinectDepthSettings::parse("fit", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        require(close_to(settings.skeleton_stretch, 1.0f), "the default skeleton stretch is 1.0");
        sfr::KinectDepthViewer viewer(sfr::KinectDepthSettings::parse("fit", nullptr, nullptr, nullptr, nullptr, nullptr, "depth"));
        viewer.set_fit_scale(1.379f);
        viewer.body_frame(frame, 1.0);
        require(viewer.settings().skeleton_stretch == 0.0f && same_view(viewer.skeleton_view(), viewer.view()),
                "\"depth\" stretches the skeleton with the depth's k, as before");
        require(close_to(sfr::KinectDepthSettings::parse("fit", nullptr, nullptr, nullptr, nullptr, nullptr, "1.6").skeleton_stretch, 1.6f) &&
                    close_to(sfr::KinectDepthSettings::parse("fit", nullptr, nullptr, nullptr, nullptr, nullptr, "7").skeleton_stretch, 2.0f) &&
                    close_to(sfr::KinectDepthSettings::parse("fit", nullptr, nullptr, nullptr, nullptr, nullptr, "x").skeleton_stretch, 1.0f),
                "other stretches up to 2.0 still work; nonsense is the default");
    }
    const auto set = sfr::KinectDepthSettings::parse("fit", nullptr, nullptr, nullptr, nullptr, nullptr, "1.0", "9");
    require(close_to(set.skeleton_stretch, 1.0f) && close_to(set.skeleton_distance, 4.0f), "parsed and clamped");
    require(close_to(sfr::KinectDepthSettings::parse("fit", nullptr, nullptr, nullptr, nullptr, nullptr, "0.5").skeleton_stretch, 1.0f),
            "the stretch is at least 1");

    // fit with the skeleton unstretched: the depth keeps k, the skeleton is
    // the sensor's; a hand's travel through a perspective divide is k times
    // as large (X / Z against X / (k Z)).
    sfr::KinectDepthViewer fit(set);
    fit.set_fit_scale(1.379f);
    fit.body_frame(frame, 1.0);
    const auto skeleton = fit.skeleton_view();
    require(close_to(fit.view().z_scale, 1.379f) && close_to(skeleton.z_scale, 1.0f), "depth stretched, skeleton not");
    float x_depth = 0, x_skeleton = 0, y = 0;
    const std::array<float, 3> hand{0.3f, 0.1f, 1.6f};
    sfr::kinect_v1_depth_project(sfr::kinect_depth_view_point(fit.view(), hand), x_depth, y);
    sfr::kinect_v1_depth_project(sfr::kinect_depth_view_point(skeleton, hand), x_skeleton, y);
    require(close_to((x_skeleton - 160.0f) / (x_depth - 160.0f), 1.379f, 1e-3f), "cursor travel scaled by Z / Z'");
    sfr::KinectFrame moved = frame;
    sfr::kinect_depth_view_frame(skeleton, moved);
    require(moved.bodies[0].position == frame.bodies[0].position, "the skeleton in the sensor's own coordinates");
    const auto back = sfr::kinect_depth_view_unpoint(skeleton, sfr::kinect_depth_view_point(skeleton, hand));
    require(close_to(back[2], hand[2]), "the preview's way back");

    // ideal with the skeleton's camera at 2.0 m: the depth keeps 2.5 m.
    const auto ideal_set = sfr::KinectDepthSettings::parse("ideal", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "2.0");
    sfr::KinectDepthViewer ideal(ideal_set);
    ideal.body_frame(frame, 1.0);
    ideal.depth_frame(1.0);
    const auto ideal_skeleton = ideal.skeleton_view();
    const auto in_depth = sfr::kinect_depth_view_point(ideal.view(), frame.bodies[0].position);
    const auto in_skeleton = sfr::kinect_depth_view_point(ideal_skeleton, frame.bodies[0].position);
    require(close_to(in_depth[2], 2.5f, 1e-4f) && close_to(in_skeleton[2], 2.0f, 1e-4f) &&
                close_to(in_depth[0], in_skeleton[0]) && close_to(in_depth[1], in_skeleton[1]),
            "only the skeleton's camera comes closer");
    const auto ideal_back = sfr::kinect_depth_view_unpoint(ideal_skeleton, in_skeleton);
    require(close_to(ideal_back[0], frame.bodies[0].position[0], 1e-5f) && close_to(ideal_back[2], frame.bodies[0].position[2], 1e-5f),
            "undone for the preview");
}

// A synthetic raw recording: a wall, a player moving across it, a tilted
// floor, the typical lens's rays as the table.
sfr::KinectRawHeader synthetic_header() {
    sfr::KinectRawHeader header;
    header.rays_from = sfr::KinectRawRays::sensor_table;
    header.lens = {366.0f, 366.0f, 256.0f, 212.0f, 0, 0, 0};
    sfr::kinect_v2_depth_rays_from_lens(366.0f, 366.0f, 256.0f, 212.0f, 512, 424, header.rays);
    return header;
}

sfr::KinectRawFrame synthetic_frame(int i) {
    constexpr uint32_t width = 512, height = 424;
    sfr::KinectRawFrame frame;
    frame.steady_us = 1000000 + int64_t(i) * 33333;
    frame.depth_time = 50000000 + int64_t(i) * 333333;
    // A body frame most times, and some depth images without a new one.
    frame.body_time = i % 7 == 3 ? 50000000 + int64_t(i - 1) * 333333 - 1000 : 50000000 + int64_t(i) * 333333 - 1000;
    frame.number = uint64_t(i) + 1;
    frame.flags = i % 11 == 5 ? 0 : sfr::kinect_raw_flag_mapped;
    frame.floor = tilted_floor(0.18f, 0.02f, 1.3f);
    frame.depth.assign(size_t(width) * height, 0);
    frame.body_index.assign(size_t(width) * height, sfr::kinect_v2_no_body);
    const int left = 180 + 3 * (i % 40), top = 60 + (i % 9), right = left + 110, bottom = 400 - (i % 5);
    const float person_z = 1.9f + 0.01f * float(i % 13);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            const size_t at = size_t(y) * width + x;
            if ((x * 7 + y * 3 + uint32_t(i)) % 97 == 0) continue;  // holes
            const bool person = int(x) >= left && int(x) < right && int(y) >= top && int(y) < bottom;
            frame.depth[at] = uint16_t(person ? std::lround(person_z * 1000.0f) + (x % 5) : 3400 + (y % 50));
            if (person) frame.body_index[at] = 0;
        }
    if (i % 23 < 20) {
        sfr::KinectRawBody& body = frame.bodies.emplace_back();
        body.tracking_id = 77;
        body.body_index = 0;
        body.spine_state = 2;
        // Its SpineBase through the same lens (the middle of the box).
        const float cx = (left + right) / 2.0f, cy = (top + bottom) / 2.0f + 30.0f;
        body.spine_base = {(cx - 256.0f) * person_z / 366.0f, -(cy - 212.0f) * person_z / 366.0f, person_z};
        if (i > 30) {
            sfr::KinectRawBody& second = frame.bodies.emplace_back(body);
            second.tracking_id = 78;
            second.body_index = 3;
            second.spine_base[0] += 0.7f;
        }
    }
    return frame;
}

std::string temp_file(const char* name) { return (std::filesystem::temp_directory_path() / name).string(); }

bool write_file(const std::string& path, const std::string& bytes) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    std::fclose(file);
    return ok;
}

void raw_runs_round_trip() {
    std::vector<uint8_t> pixels(200000, 255);  // longer than one run can hold
    for (size_t i = 70000; i < 70100; ++i) pixels[i] = 2;
    pixels[199999] = 0;
    std::vector<std::pair<uint8_t, uint16_t>> runs;
    sfr::kinect_raw_runs_encode(pixels.data(), pixels.size(), runs);
    require(runs.size() == 6 && runs[0] == std::make_pair(uint8_t(255), uint16_t(65535)), "runs of at most 65535");
    std::vector<uint8_t> back;
    require(sfr::kinect_raw_runs_decode(runs, pixels.size(), back) && back == pixels, "runs decode to the pixels");
    require(!sfr::kinect_raw_runs_decode(runs, pixels.size() - 1, back), "too many pixels is broken");
    runs.back().second = 0;
    require(!sfr::kinect_raw_runs_decode(runs, pixels.size(), back), "an empty run is broken");
}

void raw_file_round_trip() {
    const sfr::KinectRawHeader header = synthetic_header();
    std::string bytes = sfr::kinect_raw_file_header(header);
    require(bytes.size() == 64 + 8 * 512 * 424 && bytes.compare(0, 8, "SFRKRAW1") == 0, "header size and magic");
    std::vector<sfr::KinectRawFrame> frames;
    for (int i = 0; i < 3; ++i) {
        frames.push_back(synthetic_frame(i + 30));
        bytes += sfr::kinect_raw_encode_frame(frames.back());
    }
    // A frame cut short at the end.
    const std::string last = sfr::kinect_raw_encode_frame(synthetic_frame(40));
    bytes += last.substr(0, last.size() / 2);
    const std::string path = temp_file("sfr_kinect_raw_round_trip.bin");
    require(write_file(path, bytes), "written");
    sfr::KinectRawReader reader;
    require(reader.open(path), "opened");
    require(reader.header().width == 512 && reader.header().height == 424 &&
                reader.header().rays_from == sfr::KinectRawRays::sensor_table && reader.header().lens == header.lens &&
                reader.header().rays == header.rays,
            "the header and its rays back");
    sfr::KinectRawFrame frame;
    for (const auto& given : frames) {
        require(reader.next(frame), "a frame back");
        require(frame.steady_us == given.steady_us && frame.depth_time == given.depth_time &&
                    frame.body_time == given.body_time && frame.number == given.number && frame.flags == given.flags &&
                    frame.floor == given.floor,
                "its clocks, number, flags and floor");
        require(frame.bodies.size() == given.bodies.size(), "its bodies");
        for (size_t b = 0; b < given.bodies.size(); ++b)
            require(frame.bodies[b].tracking_id == given.bodies[b].tracking_id &&
                        frame.bodies[b].body_index == given.bodies[b].body_index &&
                        frame.bodies[b].spine_state == given.bodies[b].spine_state &&
                        frame.bodies[b].spine_base == given.bodies[b].spine_base && frame.bodies[b].tracked == 1,
                    "each body");
        require(frame.depth == given.depth && frame.body_index == given.body_index, "depth and body index");
    }
    require(!reader.next(frame) && reader.truncated() && reader.error().empty(), "the cut frame left out");
    reader.close();
    std::filesystem::remove(path);
    // Not a raw file.
    const std::string other = temp_file("sfr_kinect_raw_other.bin");
    require(write_file(other, std::string("SFRKDEP2") + std::string(100, '\0')), "written");
    require(!reader.open(other) && !reader.error().empty(), "another file is refused");
    reader.close();
    std::filesystem::remove(other);
}

void raw_rays_orientation() {
    sfr::KinectRawHeader header = synthetic_header();
    bool flip_x = true, flip_y = true;
    require(sfr::kinect_raw_rays_orientation(header, flip_x, flip_y) && !flip_x && !flip_y, "camera space");
    for (size_t i = 1; i < header.rays.size(); i += 2) header.rays[i] = -header.rays[i];
    require(sfr::kinect_raw_rays_orientation(header, flip_x, flip_y) && !flip_x && flip_y, "y down is seen");
    header.rays.assign(header.rays.size(), 0.0f);
    require(!sfr::kinect_raw_rays_orientation(header, flip_x, flip_y), "zeros are unusable");
}

void resynth_equals_online() {
    const sfr::KinectRawHeader header = synthetic_header();
    for (const char* name : {"fit", "sensor", "player", "ideal"}) {
        const sfr::KinectDepthSettings settings = sfr::KinectDepthSettings::parse(name, nullptr, nullptr);
        // Online, as the live sensor goes: body frames as they come, then each
        // depth image through the viewer, points from the mapper (here the
        // table's rays, or the typical lens when it had none), and the
        // recorder's frame from what it used.
        sfr::KinectDepthViewer viewer(settings);
        viewer.set_fit_scale(sfr::kinect_v2_fit_scale(header.lens[1]));
        std::string bytes = sfr::kinect_raw_file_header(header);
        std::vector<sfr::KinectImage> online;
        int64_t last_body = 0;
        std::vector<float> points;
        for (int i = 0; i < 120; ++i) {
            const sfr::KinectRawFrame given = synthetic_frame(i);
            if (given.body_time != last_body) {
                sfr::KinectFrame body_frame;
                body_frame.floor_plane = given.floor;
                for (const auto& raw : given.bodies) {
                    sfr::KinectBody& body = body_frame.bodies.emplace_back();
                    body.tracking_id = uint32_t(raw.tracking_id);
                    body.sensor_index = raw.body_index;
                    body.position = raw.spine_base;
                    body.joint_states[0] = raw.spine_state;
                    body.joints[5] = {1, 2, 3};  // what the view does not keep
                }
                viewer.body_frame(body_frame, double(given.body_time) * 1e-7);
                last_body = given.body_time;
            }
            const sfr::KinectDepthView view = viewer.depth_frame(double(given.depth_time) * 1e-7);
            if (given.flags & sfr::kinect_raw_flag_mapped)
                sfr::kinect_v2_depth_points_from_rays(given.depth.data(), header.rays.data(), given.depth.size(), points);
            else
                sfr::kinect_v2_depth_points_from_lens(given.depth.data(), 512, 424, points);
            sfr::KinectImage made;
            sfr::kinect_v2_synthesize_depth(points.data(), given.body_index.data(), given.depth.size(), view, made);
            online.push_back(made);
            sfr::KinectRawFrame recorded = given;
            sfr::kinect_raw_set_bodies(viewer.bodies(), recorded);
            bytes += sfr::kinect_raw_encode_frame(recorded);
        }
        const std::string path = temp_file("sfr_kinect_raw_resynth.bin");
        require(write_file(path, bytes), "written");
        sfr::KinectRawReader reader;
        require(reader.open(path), "opened");
        sfr::KinectV2Resynth resynth(settings, reader.header());
        require(resynth.warning().empty(), "the rays are fine");
        sfr::KinectRawFrame frame;
        sfr::KinectImage image;
        size_t index = 0;
        uint32_t players = 0;
        while (reader.next(frame)) {
            resynth.make(frame, image);
            require(index < online.size() && image.pixels == online[index].pixels,
                    (std::string(name) + ": the offline image is the online one, bit for bit").c_str());
            require(image.number == frame.number && image.steady_us == frame.steady_us, "its number and clock");
            for (size_t p = 0; p < image.pixels.size(); p += 2) players += (image.pixels[p] & 7) ? 1 : 0;
            ++index;
        }
        require(index == online.size(), "every frame");
        require(players > 0, (std::string(name) + ": the player is in the images").c_str());
        reader.close();
        std::filesystem::remove(path);
    }
}

// The launcher's choice becomes what the game would read from the same values.
void choice_is_the_games_settings() {
    sfr::KinectDepthChoice choice;
    choice.distance = 3.0f;
    choice.height = 0.95f;
    choice.follow = 3.5f;
    choice.deadband = 0.0f;
    choice.skeleton_distance = 1.8f;
    const auto made = sfr::kinect_depth_settings(choice);
    const auto read = sfr::KinectDepthSettings::parse("ideal", "3.00", "0.95", nullptr, "3.50", "0.00", nullptr, "1.80");
    require(made.mode == read.mode && made.distance == read.distance && made.height == read.height &&
                made.follow == read.follow && made.deadband == read.deadband &&
                made.skeleton_distance == read.skeleton_distance && made.skeleton_stretch == read.skeleton_stretch &&
                made.fit_scale == read.fit_scale,
            "Recompiled: the same settings as the game's environment");
    const auto defaults = sfr::kinect_depth_settings({});
    require(defaults.mode == sfr::KinectDepthViewMode::ideal && close_to(defaults.height, 1.0f) &&
                close_to(defaults.distance, 2.5f) && defaults.skeleton_distance == 0,
            "the default choice is ideal's defaults");
    choice.view = sfr::KinectDepthChoiceView::fit;
    const auto fit = sfr::kinect_depth_settings(choice);
    require(fit.mode == sfr::KinectDepthViewMode::fit && !fit.player_view && fit.fit_scale == 0 &&
                fit.skeleton_stretch == 1.0f && close_to(fit.distance, 3.0f),
            "fit, with the lens's stretch and the tuning kept");
    choice.view = sfr::KinectDepthChoiceView::sensor;
    require(sfr::kinect_depth_settings(choice).mode == sfr::KinectDepthViewMode::sensor, "sensor");
    choice.distance = 9.0f;
    require(close_to(sfr::kinect_depth_settings(choice).distance, 4.0f), "clamped as the game clamps");
}

// Settings changed while running: the ideal state is kept, the views follow.
void viewer_takes_new_settings() {
    const std::array<float, 4> level{0, 1, 0, 1.0f};
    sfr::KinectDepthSettings settings = sfr::kinect_depth_settings({});
    settings.mode = sfr::KinectDepthViewMode::fit;
    sfr::KinectDepthViewer viewer(settings);
    viewer.body_frame(frame_with({{1, {0.4f, 0, 2.0f}}}, level), 0.0);
    const sfr::KinectDepthView fit_view = viewer.depth_frame(0.0);
    require(fit_view.z_scale > 1.3f && fit_view.origin == std::array<float, 3>{}, "fit makes fit");
    require(viewer.ideal().placed && close_to(viewer.ideal().reference[0], 0.4f) &&
                close_to(viewer.ideal_view().origin[2], 2.0f - 2.5f),
            "the ideal camera is kept in fit too");
    settings.mode = sfr::KinectDepthViewMode::ideal;
    settings.distance = 3.0f;
    viewer.set_settings(settings);
    require(close_to(viewer.view().origin[0], 0.4f) && close_to(viewer.view().origin[2], 2.0f - 3.0f) &&
                viewer.view().z_scale == 1.0f && viewer.view().rotated,
            "ideal at once, from the state already kept, at the new distance");
    viewer.body_frame(frame_with({{1, {0.4f, 0, 2.0f}}}, level), 1.0 / 30);
    require(close_to(viewer.depth_frame(1.0 / 30).origin[2], -1.0f), "and on");
    settings.mode = sfr::KinectDepthViewMode::sensor;
    viewer.set_settings(settings);
    require(viewer.view().origin == std::array<float, 3>{} && viewer.view().z_scale == 1.0f, "sensor");
    settings.mode = sfr::KinectDepthViewMode::fit;
    viewer.set_fit_scale(1.5f);
    viewer.set_settings(settings);
    require(close_to(viewer.view().z_scale, 1.5f), "fit keeps the stretch the lens gave");
}

// The top-down snapshot is the ideal view's own camera and the bodies in its
// levelled space.
void ideal_snapshot_is_the_view() {
    const float pitch = 10.0f * 3.14159265f / 180.0f;
    const auto floor = tilted_floor(pitch, 0, 1.2f);
    IdealRun run;
    const auto frame = frame_with({{7, {-0.5f, -0.3f, 2.0f}}, {9, {0.7f, -0.3f, 2.6f}}}, floor);
    run.step(frame);
    const auto snapshot = sfr::kinect_ideal_snapshot(frame, run.state, run.settings);
    const auto view = run.view();
    require(snapshot.valid && snapshot.floor_known && snapshot.placed && close_to(snapshot.camera_x, view.origin[0]) &&
                close_to(snapshot.camera_z, view.origin[2]) && close_to(snapshot.distance, 2.5f),
            "the camera is the view's");
    require(snapshot.players.size() == 2 && snapshot.players[0].tracking_id == 7 && snapshot.players[0].slot == -1,
            "every body, in order, no slot");
    for (size_t i = 0; i < 2; ++i) {
        const auto seen = sfr::kinect_depth_view_point(view, frame.bodies[i].position);
        require(close_to(snapshot.players[i].z - snapshot.camera_z, seen[2], 1e-4f) &&
                    close_to(snapshot.players[i].x - snapshot.camera_x, seen[0], 1e-4f),
                "a player's depth to the camera is the depth the game sees");
        require(close_to(snapshot.players[i].sensor_z, frame.bodies[i].position[2]), "and the v2's own depth");
    }
    require(close_to((snapshot.players[0].z + snapshot.players[1].z) / 2 - snapshot.camera_z, 2.5f, 1e-4f),
            "the camera is distance behind the players' mean");
    const auto nobody = sfr::kinect_ideal_snapshot(frame_with({{1, {0, 0, 0}}}), {}, run.settings);
    require(nobody.players.empty() && !nobody.placed && close_to(nobody.camera_z, 0.0f), "a point of none is nobody");
}

// The simulated v1's colour: each pixel of the depth the game gets taken
// back into the sensor's space through the very view it was made with, so
// its colour is the colour of what that pixel shows.
void v1_depth_back_to_the_sensor() {
    // A level-making turn and a camera moved off the v2, as ideal makes.
    const auto floor = tilted_floor(12.0f * 3.14159265f / 180.0f, 3.0f * 3.14159265f / 180.0f, 1.1f);
    IdealRun run;
    run.step(frame_with({{1, {0.6f, -0.2f, 1.8f}}}, floor));
    const sfr::KinectDepthView view = run.view();
    require(view.rotated && !close_to(view.origin[0], 0) && !close_to(view.origin[2], 0), "a turned, moved camera");
    // A wall 2.6 m away in the sensor's space, densely sampled, and one known
    // point on it (a player's pixel) that lands on a pixel of its own.
    Points wall;
    for (float x = -1.8f; x <= 1.8f; x += 0.004f)
        for (float y = -1.2f; y <= 1.2f; y += 0.004f) wall.add(x, y, 2.6f + 0.1f * x);
    const std::array<float, 3> known{0.35f, 0.15f, 2.6f + 0.1f * 0.35f};
    sfr::KinectImage image;
    wall.make(image, view);
    image.view_origin = view.origin;
    image.view_z_scale = view.z_scale;
    image.view_rotated = view.rotated;
    image.view_rotation = view.rotation;
    const sfr::KinectDepthView back = sfr::kinect_image_view(image);
    require(back.origin == view.origin && back.rotated && back.rotation == view.rotation, "the image's own view");
    std::vector<float> points;
    sfr::kinect_v1_depth_points(image, back, points);
    require(points.size() == size_t(320 * 240 * 3), "a point a pixel");
    size_t valid = 0;
    for (uint32_t row = 0; row < 240; ++row)
        for (uint32_t column = 0; column < 320; ++column) {
            const size_t at = size_t(row) * 320 + column;
            const float* p = &points[3 * at];
            if (depth_at(image, column, row) == 0) {
                require(p[0] == 0 && p[1] == 0 && p[2] == 0, "no value, no point");
                continue;
            }
            ++valid;
            // On the wall (to the millimetre the image keeps)...
            require(std::fabs(p[2] - (2.6f + 0.1f * p[0])) < 0.004f, "the point is on the wall");
            // ...and seen by the virtual camera on that same pixel.
            const auto seen = sfr::kinect_depth_view_point(view, {p[0], p[1], p[2]});
            float x = 0, y = 0;
            require(sfr::kinect_v1_depth_project(seen, x, y) && std::fabs(x - float(column)) < 0.51f &&
                        std::fabs(y - float(row)) < 0.51f,
                    "the point lands on its own pixel");
        }
    require(valid > 20000, "the wall is in the image");
    // The known point's pixel gives back the known point, within half a
    // pixel across, and its colour position is the known point's.
    const auto seen = sfr::kinect_depth_view_point(view, known);
    float x = 0, y = 0;
    require(sfr::kinect_v1_depth_project(seen, x, y), "the known point is in front");
    const size_t at = size_t(std::floor(y + 0.5f)) * 320 + size_t(std::floor(x + 0.5f));
    const std::array<float, 3> found{points[3 * at], points[3 * at + 1], points[3 * at + 2]};
    const float pixel = 3.3f / sfr::kinect_v1_depth_focal;  // a pixel across at that depth
    require(std::fabs(found[0] - known[0]) < pixel && std::fabs(found[1] - known[1]) < pixel &&
                std::fabs(found[2] - known[2]) < 0.005f,
            "the known point comes back");
    const auto colour_known = sfr::kinect_v2_colour_estimate(known, sfr::KinectV2View::original);
    const auto colour_found = sfr::kinect_v2_colour_estimate(found, sfr::KinectV2View::original);
    require(colour_known.visible && colour_found.visible && std::fabs(colour_known.x - colour_found.x) < 1.0f &&
                std::fabs(colour_known.y - colour_found.y) < 1.0f,
            "and the same colour pixel (640x360)");
    // Without a camera move (the sensor view) the point is the pixel's ray.
    sfr::KinectImage flat;
    Points one;
    one.at_pixel(200, 60, 2.0f);
    one.make(flat);
    sfr::kinect_v1_depth_points(flat, sfr::kinect_image_view(flat), points);
    const float* p = &points[3 * (60 * 320 + 200)];
    require(close_to(p[0], 40.0f * 2.0f / sfr::kinect_v1_depth_focal) && close_to(p[1], 60.0f * 2.0f / sfr::kinect_v1_depth_focal) &&
                close_to(p[2], 2.0f),
            "the sensor view: the pixel's own ray");
}

// Nobody tracked yet: the ideal camera is the v2's own place laterally and
// in depth, level, the height above the floor, so it sees the middle of the
// v2's view (and nothing beyond 4 m of it).
void ideal_camera_without_anyone() {
    const auto floor = tilted_floor(10.0f * 3.14159265f / 180.0f, 0, 0.9f);
    IdealRun run;
    run.step(frame_with({}, floor), 30);
    const auto view = run.view();
    require(!run.state.placed && run.state.floor_known && view.rotated, "levelled, not placed");
    require(close_to(view.origin[0], 0) && close_to(view.origin[2], 0) && close_to(view.origin[1], -0.9f + 1.0f, 1e-3f),
            "at the v2, the set height above the floor");
    // A point straight ahead of the v2 at 2 m stays in the middle column.
    float x = 0, y = 0;
    require(sfr::kinect_v1_depth_project(sfr::kinect_depth_view_point(view, {0, 0, 2.0f}), x, y) && close_to(x, 160),
            "centred left to right");
}

// A standing player (feet on the floor, head 1.7 m up, SpineBase at 0.95 m)
// 2 m in front of a v2 mounted sensor_height above the floor and pitched
// down by pitch, in the v2's own space; the ideal view at 2.5 m and 1.0 m
// must frame the whole body: the head in the top third, the feet in the
// bottom 15%.
struct Framed {
    int top = -1, bottom = -1;
};
Framed frame_a_standing_player(float pitch_degrees, float sensor_height, const std::array<float, 4>& floor_given,
                               int steps = 30) {
    const float pitch = pitch_degrees * 3.14159265f / 180.0f;
    // A point of the room (x, height above the floor, distance along the
    // floor) in the sensor's space: its axes are x, up (0, cos, sin) and
    // forward (0, -sin, cos) seen from the room.
    const auto sensor_point = [&](float x, float above_floor, float forward) {
        const float y = above_floor - sensor_height;
        return std::array<float, 3>{x, y * std::cos(pitch) + forward * std::sin(pitch),
                                    -y * std::sin(pitch) + forward * std::cos(pitch)};
    };
    IdealRun run;
    const auto spine = sensor_point(0.1f, 0.95f, 2.0f);
    for (int i = 0; i < steps; ++i) run.step(frame_with({{1, spine}}, floor_given));
    Points body;
    for (float h = 0.0f; h <= 1.7001f; h += 0.005f)
        for (float x = -0.1f; x <= 0.3001f; x += 0.005f) {
            const auto p = sensor_point(x, h, 2.0f);
            body.add(p[0], p[1], p[2], 0);
        }
    sfr::KinectImage image;
    body.make(image, run.view());
    Framed framed;
    for (int row = 0; row < 240; ++row)
        for (int column = 0; column < 320; ++column)
            if (depth_at(image, uint32_t(column), uint32_t(row)) & 7) {
                if (framed.top < 0) framed.top = row;
                framed.bottom = row;
            }
    return framed;
}

void ideal_frames_a_standing_player() {
    for (const float pitch : {0.0f, 10.0f, -10.0f}) {
        const float rad = pitch * 3.14159265f / 180.0f;
        // The v2's FloorClipPlane: up in its space, its height as W.
        const std::array<float, 4> floor{0, std::cos(rad), -std::sin(rad), 1.0f};
        const Framed framed = frame_a_standing_player(pitch, 1.0f, floor);
        require(framed.top >= 0 && framed.top < 80, "the head in the top third");
        require(framed.bottom >= 204 && framed.bottom <= 239, "the feet in the bottom 15%, inside the picture");
        // A sensor low on the floor, looking up a little.
        const Framed low = frame_a_standing_player(pitch, 0.3f, {0, std::cos(rad), -std::sin(rad), 0.3f});
        require(std::abs(low.top - framed.top) <= 1 && std::abs(low.bottom - framed.bottom) <= 1,
                "the camera's height is the floor's, not the sensor's");
    }
    // A v2 that knows the floor's direction but not yet its distance (W 0):
    // the camera at the sensor's own height, never a metre above it.
    const std::array<float, 4> no_height{0, 1, 0, 0};
    const Framed unknown = frame_a_standing_player(0, 1.0f, no_height);
    require(unknown.top >= 0 && unknown.top < 80 && unknown.bottom >= 204, "a sensor at 1 m frames the same without W");
    const Framed low_unknown = frame_a_standing_player(0, 0.7f, no_height);
    require(low_unknown.top >= 0 && low_unknown.top < 80 && low_unknown.bottom <= 239,
            "a lower sensor without W still shows the head and the feet");
    IdealRun run;
    run.step(frame_with({{1, {0, -0.05f, 2.0f}}}, no_height), 30);
    require(run.state.floor_known && !run.state.height_known && close_to(run.view().origin[1], 0), "levelled, no height yet");
    run.step(frame_with({{1, {0, -0.05f, 2.0f}}}, {0, 1, 0, 0.7f}));
    require(run.state.height_known && close_to(run.state.floor[3], 0.7f) && close_to(run.view().origin[1], 0.3f),
            "the first distance is taken at once");
    run.step(frame_with({{1, {0, -0.05f, 2.0f}}}, no_height), 30);
    require(run.state.height_known && close_to(run.state.floor[3], 0.7f), "a plane without one later leaves it");
}

void colour_sampled_per_pixel() {
    const std::vector<uint32_t> colour{1, 2, 3, 4, 5, 6};  // 3 x 2
    const float positions[] = {0.2f, 0.7f, 2.9f, 1.0f, NAN, 0.0f, 3.0f, 0.0f, -0.1f, 0.5f, 1.5f, 0.5f};
    std::vector<uint32_t> out;
    sfr::kinect_colour_from_positions(positions, 6, colour.data(), 3, 2, out);
    require(out == std::vector<uint32_t>{1, 6, 0, 0, 0, 2}, "the nearest pixel, black for none or outside");
}
}

int main() {
    const std::pair<const char*, void (*)()> cases[] = {
        {"v1 depth back", v1_depth_back_to_the_sensor},
        {"colour per pixel", colour_sampled_per_pixel},
        {"ideal without anyone", ideal_camera_without_anyone},
        {"ideal frames a standing player", ideal_frames_a_standing_player},
        {"choice", choice_is_the_games_settings},
        {"viewer settings", viewer_takes_new_settings},
        {"ideal snapshot", ideal_snapshot_is_the_view},
        {"packing", packing_is_the_v1s},
        {"projection", projection_is_the_nominal_lens},
        {"points land", points_land_where_projected},
        {"z-buffer", nearest_wins},
        {"hole filling", holes_filled_conservatively},
        {"special values", out_of_range_is_no_value},
        {"view transform", view_moves_the_camera},
        {"settings", settings_from_text},
        {"latch holds", latch_holds_still},
        {"latch mean", latch_settles_on_the_mean},
        {"latch height", latch_height_from_the_floor},
        {"fit scale", fit_scale_from_the_lens},
        {"fit whole view", fit_shows_the_whole_v2_view},
        {"fit depth and size", fit_depth_matches_size},
        {"fit range", fit_range_clamps},
        {"fit skeleton and floor", fit_moves_skeleton_and_floor},
        {"fit of one", fit_of_one_is_sensor},
        {"sensor view", sensor_view_stays},
        {"lens fallback", lens_fallback_matches_estimate},
        {"levelling", levelling_takes_out_a_known_tilt},
        {"ideal settings", ideal_settings_from_text},
        {"ideal follow", ideal_follows_slowly},
        {"ideal two players", ideal_two_players},
        {"ideal determinism", ideal_is_deterministic},
        {"ideal without a floor", ideal_without_a_floor},
        {"ideal is a true v1", ideal_is_a_true_v1},
        {"ideal skeleton and floor", ideal_moves_skeleton_and_floor},
        {"viewer keeps the old views", viewer_keeps_the_old_views},
        {"raw runs", raw_runs_round_trip},
        {"raw file", raw_file_round_trip},
        {"raw rays", raw_rays_orientation},
        {"resynth equals online", resynth_equals_online},
        {"skeleton knobs", skeleton_knobs},
    };
    unsigned failures = 0;
    for (const auto& [name, test] : cases) {
        try {
            test();
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    if (failures) return 1;
    std::cout << "kinect_v2_depth: all passed\n";
    return 0;
}
