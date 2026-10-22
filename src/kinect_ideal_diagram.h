#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace sfr {

// The Kinect v2 depth view as the launcher chooses it, for the preview it
// opens (kinect_preview.h): which view, and Recompiled's tuning in metres and
// seconds (the launcher keeps centimetres and tenths: LauncherSettings).
// Plain data, so the launcher holds it on every platform; a Windows sensor
// turns it into its KinectDepthSettings (kinect_depth_settings,
// kinect_v2_depth.h), the same values the game reads from
// SFR_KINECT_V2_DEPTH_*.
enum class KinectDepthChoiceView : uint8_t { ideal, fit, sensor };
struct KinectDepthChoice {
    KinectDepthChoiceView view = KinectDepthChoiceView::ideal;
    float distance = 2.5f, height = 1.0f, follow = 2.0f, deadband = 0.15f;
    float skeleton_distance = 0;  // 0: the depth's distance
    bool operator==(const KinectDepthChoice&) const = default;
};
// "ideal", "fit" or "sensor" (SFR_KINECT_V2_DEPTH_VIEW's names).
const char* kinect_depth_choice_name(KinectDepthChoiceView view);

// Where the Recompiled (ideal) view's virtual v1 stands and where the
// players are, top-down, as the depth synthesis itself has them
// (kinect_ideal_snapshot, kinect_v2_depth.h): x and z in metres, in the
// levelled space the ideal view works in (the v2's own camera space turned so
// the floor is level; the v2's own space while no floor is known). The v2
// stands at the origin of that space looking along +z.
struct KinectIdealPlayer {
    uint32_t tracking_id = 0;
    int slot = -1;           // the title's player: 0 for 1P, 1 for 2P, -1 neither
    float x = 0, z = 0;      // SpineBase, levelled space
    float sensor_z = 0;      // the v2's own depth to it (its camera space z)
};
struct KinectIdealSnapshot {
    bool valid = false;      // false: no sensor, or one that makes no depth
    bool floor_known = false, placed = false;
    float camera_x = 0, camera_z = 0;   // the virtual v1, levelled space
    float distance = 2.5f;              // from the camera to the players' depth line
    std::vector<KinectIdealPlayer> players;
};

// The two cameras' horizontal fields of view, and the distances each measures.
constexpr float kinect_v2_horizontal_fov_degrees = 70.6f, kinect_v1_horizontal_fov_degrees = 57.0f;
constexpr float kinect_v1_near_metres = 0.8f, kinect_v1_far_metres = 4.0f;
constexpr float kinect_v2_near_metres = 0.5f, kinect_v2_far_metres = 4.5f;

// The top-down drawing of a snapshot in a width x height area (pixels, (0, 0)
// its top left; y down): the v2 at the bottom looking up, the virtual v1
// behind or in front of it, the players. Pure arithmetic shared by the
// preview window (GDI) and the launcher (ImGui), and tested
// (kinect_ideal_diagram_test). x is drawn as the depth images draw it (the
// camera space's +x to the right), so the drawing agrees with the preview's
// pictures.
struct KinectDiagramPoint {
    float x = 0, y = 0;
};
struct KinectDiagramPlayer {
    KinectDiagramPoint at;
    int slot = -1;
    float v1_depth = 0;      // to the virtual v1, along its axis: the depth the game sees
    float v2_depth = 0;      // to the real v2 (its own z)
    bool in_v1_view = false;   // inside the virtual v1's horizontal field of view
    bool in_v1_range = false;  // and 0.8..4.0 m from it
    bool in_v2_view = false;   // inside the real v2's field of view and range
    // The part of the players' row both cameras see at this player's depth
    // (pixels; left > right when none).
    float band_left = 0, band_right = 0;
};
// A ruler mark: where it is (y for the depth ruler, x for the lateral one;
// pixels), its distance in metres, and whether it is a whole metre (a
// label and a faint grid line) or a half.
struct KinectDiagramTick {
    float at = 0;
    float metres = 0;
    bool whole = false;
};
struct KinectIdealDiagram {
    float scale = 0;  // pixels a metre; 0 when nothing fits
    // The rulers: depth from the real v2 (its z) up the left edge, every
    // 0.5 m over the drawing's whole height; across along the bottom (x, 0
    // on the v2's axis), every 0.5 m over its whole width. They sit in the
    // strips the caller reserved (ruler_left, ruler_bottom) beside the drawing.
    std::vector<KinectDiagramTick> depth_ticks, lateral_ticks;
    // The virtual v1's usable depths on its own axis: 0.8 m and 4.0 m from it.
    KinectDiagramPoint v1_near_mark, v1_far_mark;
    // Each camera's field of view: the apex (the camera) and the two far
    // corners at the drawing's far edge (beyond the area at the sides; clip).
    std::array<KinectDiagramPoint, 3> v2_view{}, v1_view{};
    // The virtual v1's usable depths (0.8 and 4.0 m) across its view: y and
    // each line's ends.
    float v1_near_y = 0, v1_far_y = 0;
    std::array<float, 2> v1_near_x{}, v1_far_x{};
    // The players' depth line (the camera stands distance behind it).
    float players_y = 0;
    std::vector<KinectDiagramPlayer> players;
    // A point of the levelled space (metres) in the drawing.
    float origin_x = 0, origin_y = 0, z_low = 0;
    KinectDiagramPoint place(float x, float z) const { return {origin_x + x * scale, origin_y - (z - z_low) * scale}; }
};
// At least +-2.5 m wide and from the nearer camera to 4.5 m deep, grown so
// the cameras, the players' line and every player fit, with room under the
// lower camera for its label. ruler_left and ruler_bottom are pixels kept
// free at the left and bottom edges for the rulers' labels: the drawing is
// the rest.
KinectIdealDiagram kinect_ideal_diagram(const KinectIdealSnapshot& snapshot, float width, float height,
                                        float ruler_left = 0, float ruler_bottom = 0);

}
