#include "kinect_ideal_diagram.h"

#include <algorithm>
#include <cmath>

namespace sfr {
namespace {
float half_tangent(float degrees) { return float(std::tan(double(degrees) * 3.14159265358979323846 / 360.0)); }
}

const char* kinect_depth_choice_name(KinectDepthChoiceView view) {
    switch (view) {
    case KinectDepthChoiceView::fit: return "fit";
    case KinectDepthChoiceView::sensor: return "sensor";
    case KinectDepthChoiceView::ideal: break;
    }
    return "ideal";
}

KinectIdealDiagram kinect_ideal_diagram(const KinectIdealSnapshot& snapshot, float full_width, float full_height,
                                        float ruler_left, float ruler_bottom) {
    KinectIdealDiagram diagram;
    ruler_left = std::max(0.0f, ruler_left);
    ruler_bottom = std::max(0.0f, ruler_bottom);
    const float width = full_width - ruler_left, height = full_height - ruler_bottom;
    if (!(width > 0) || !(height > 0)) return diagram;
    const float v1_tangent = half_tangent(kinect_v1_horizontal_fov_degrees);
    const float v2_tangent = half_tangent(kinect_v2_horizontal_fov_degrees);
    const float camera_x = snapshot.camera_x, camera_z = snapshot.camera_z;
    const float players_z = camera_z + snapshot.distance;
    // What must fit: both cameras, the players' line and every player, and
    // at least 2.5 m to each side and 4 m deep.
    float half_width = std::max(2.5f, std::fabs(camera_x) + 0.6f);
    float z_low = std::min(0.0f, camera_z), z_high = std::max({4.5f, players_z + 0.5f, z_low + 2.5f});
    for (const KinectIdealPlayer& player : snapshot.players) {
        if (!std::isfinite(player.x) || !std::isfinite(player.z)) continue;
        half_width = std::max(half_width, std::fabs(player.x) + 0.4f);
        z_low = std::min(z_low, player.z);
        z_high = std::max(z_high, player.z + 0.5f);
    }
    half_width = std::min(half_width, 8.0f);
    z_high = std::min(z_high, z_low + 10.0f);
    // Room under the lower camera for its label.
    z_low -= 0.45f;
    diagram.scale = std::min(width / (2 * half_width), height / (z_high - z_low));
    diagram.z_low = z_low;
    diagram.origin_x = ruler_left + width / 2;
    // The slack goes above, so the cameras stay at the bottom.
    diagram.origin_y = height;
    diagram.v1_near_mark = diagram.place(camera_x, camera_z + kinect_v1_near_metres);
    diagram.v1_far_mark = diagram.place(camera_x, camera_z + kinect_v1_far_metres);
    // The rulers, every half metre over the whole drawing.
    const float drawn_low = z_low, drawn_high = z_low + height / diagram.scale;
    for (int half = int(std::ceil(drawn_low * 2 - 1e-4f)); half <= int(std::floor(drawn_high * 2 + 1e-4f)); ++half)
        diagram.depth_ticks.push_back({diagram.place(0, half * 0.5f).y, half * 0.5f, half % 2 == 0});
    const float drawn_side = width / 2 / diagram.scale;
    for (int half = -int(std::floor(drawn_side * 2 + 1e-4f)); half <= int(std::floor(drawn_side * 2 + 1e-4f)); ++half)
        diagram.lateral_ticks.push_back({diagram.place(half * 0.5f, 0).x, half * 0.5f, half % 2 == 0});
    const auto place = [&](float x, float z) { return diagram.place(x, z); };
    const float far_z = z_low + height / diagram.scale;  // the drawing's top edge
    diagram.v2_view = {place(0, 0), place(-v2_tangent * far_z, far_z), place(v2_tangent * far_z, far_z)};
    const float v1_reach = std::max(0.0f, far_z - camera_z);
    diagram.v1_view = {place(camera_x, camera_z), place(camera_x - v1_tangent * v1_reach, camera_z + v1_reach),
                       place(camera_x + v1_tangent * v1_reach, camera_z + v1_reach)};
    diagram.v1_near_y = place(0, camera_z + kinect_v1_near_metres).y;
    diagram.v1_far_y = place(0, camera_z + kinect_v1_far_metres).y;
    diagram.v1_near_x = {place(camera_x - v1_tangent * kinect_v1_near_metres, 0).x,
                         place(camera_x + v1_tangent * kinect_v1_near_metres, 0).x};
    diagram.v1_far_x = {place(camera_x - v1_tangent * kinect_v1_far_metres, 0).x,
                        place(camera_x + v1_tangent * kinect_v1_far_metres, 0).x};
    diagram.players_y = place(0, players_z).y;
    for (const KinectIdealPlayer& player : snapshot.players) {
        if (!std::isfinite(player.x) || !std::isfinite(player.z)) continue;
        KinectDiagramPlayer& drawn = diagram.players.emplace_back();
        drawn.at = place(player.x, player.z);
        drawn.slot = player.slot;
        drawn.v1_depth = player.z - camera_z;
        drawn.v2_depth = player.sensor_z;
        drawn.in_v1_view = drawn.v1_depth > 0 && std::fabs(player.x - camera_x) <= v1_tangent * drawn.v1_depth;
        drawn.in_v1_range = drawn.v1_depth >= kinect_v1_near_metres && drawn.v1_depth <= kinect_v1_far_metres;
        drawn.in_v2_view = player.z > 0 && std::fabs(player.x) <= v2_tangent * player.z &&
                           player.sensor_z >= kinect_v2_near_metres && player.sensor_z <= kinect_v2_far_metres;
        // Both cameras' views at this depth.
        const float v1_half = std::max(0.0f, v1_tangent * drawn.v1_depth), v2_half = std::max(0.0f, v2_tangent * player.z);
        const float left = std::max(camera_x - v1_half, -v2_half), right = std::min(camera_x + v1_half, v2_half);
        drawn.band_left = place(left, 0).x;
        drawn.band_right = place(right, 0).x;
        if (!(right > left) || drawn.v1_depth <= 0) drawn.band_left = drawn.band_right + 1;  // seen by neither
    }
    return diagram;
}

}
