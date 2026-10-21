#pragma once
#include "kinect_ideal_diagram.h"
#include "kinect_sensor.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace sfr {

// A Kinect v2 standing in for a Kinect v1's depth camera, for the game's own
// depth view (docs/kinect-sensor.md, "Kinect v2 深度").
// On the console the race's lean, crouch, jump and side step are decided by
// the title's code reading the 320x240 depth-and-player-index image, not the
// skeleton. A v1 on the PC hands its image over as it is; a v2's depth camera
// is another camera (512x424, a wider lens, another position for the
// player), so its depth is not resampled but reprojected: every v2 depth
// pixel becomes a point in camera space (the sensor's own coordinate mapper),
// is moved into a virtual v1 depth camera (KinectDepthView) and projected
// through the v1's nominal lens, the nearest point winning each pixel.
//
// None of this is Windows', so it is tested everywhere (kinect_v2_depth_test).

// The virtual v1 depth camera: NuiImageCamera.h's nominal focal length at
// 320x240 (NUI_CAMERA_DEPTH_NOMINAL_FOCAL_LENGTH_IN_PIXELS), centred, as
// NuiTransformSkeletonToDepthImage projects (NuiSkeleton.h):
// x = 160 + X * f / Z, y = 120 - Y * f / Z, pixel x at x (rounded).
constexpr uint32_t kinect_v1_depth_width = 320, kinect_v1_depth_height = 240;
constexpr float kinect_v1_depth_focal = 285.63f;
constexpr float kinect_v1_depth_centre_x = 160.0f, kinect_v1_depth_centre_y = 120.0f;
// NuiImageCamera.h: the default range a v1 measures (NUI_IMAGE_DEPTH_MINIMUM
// and _MAXIMUM, in millimetres before the shift), and the player index's
// three bits (NUI_IMAGE_PLAYER_INDEX_SHIFT).
constexpr uint32_t kinect_v1_depth_minimum_mm = 800, kinect_v1_depth_maximum_mm = 4000;
constexpr uint32_t kinect_v1_player_index_shift = 3;
// A pixel without a depth. A v1 stream opened without
// NUI_IMAGE_STREAM_FLAG_DISTINCT_OVERFLOW_DEPTH_VALUES (as ours is, flags 0)
// reports too near, too far and unknown all as NUI_IMAGE_DEPTH_NO_VALUE (0),
// player 0; the distinct values (0x0FFF << 3 too far, 0x1FFF << 3 unknown,
// 0 too near) are only for that flag.
constexpr uint16_t kinect_v1_depth_no_value = 0;

// One v1 depth-and-player-index pixel: the millimetres shifted up three, the
// player (1..6, the skeleton slot + 1; 0 none) in the low three bits; outside
// the v1's range, no value.
uint16_t kinect_v1_depth_pixel(uint32_t millimetres, uint32_t player);

// A camera-space point (metres) through the v1's nominal depth lens: false
// for a point at or behind the camera.
bool kinect_v1_depth_project(const std::array<float, 3>& point, float& x, float& y);

// Where the virtual v1 stands, in the v2's camera space: it is the v2 moved
// by origin (no turn), and its depth is stretched by z_scale, so a point p
// is (p - origin) with z times z_scale to it. The fit view (origin zero,
// z_scale k) is the v2's whole field of view through the v1's lens: a point
// at angle a is drawn at the v1 pixel for angle a / k, and a person at d
// looks like one of the same size at k d, with that depth.
// The ideal view also turns: when rotated, a point is first turned by
// rotation (row-major 3x3, kinect_level_rotation: the floor made level) and
// then moved, R p - origin; origin is then in that levelled space.
struct KinectDepthView {
    std::array<float, 3> origin{};
    float z_scale = 1.0f;
    bool rotated = false;
    std::array<float, 9> rotation{1, 0, 0, 0, 1, 0, 0, 0, 1};
};
std::array<float, 3> kinect_depth_view_point(const KinectDepthView& view, const std::array<float, 3>& p);
// The other way: a point the virtual camera sees, in the v2's space.
std::array<float, 3> kinect_depth_view_unpoint(const KinectDepthView& view, const std::array<float, 3>& p);
// A frame's skeletons (centres, joints, a v2's fingertips and thumbs; a
// point left at exactly zero means none and stays so) and floor plane into
// the virtual camera, so that skeleton and depth image agree. The floor plane
// goes through the same space (its normal turned with a rotated view and
// renormalised under a stretch), and a stretched frame's gravity is the new
// floor normal, as a v2's is the floor's; without a stretch gravity is a
// direction: turned with a rotated view, otherwise as it was.
void kinect_depth_view_frame(const KinectDepthView& view, KinectFrame& frame);

// A floor plane the sensor actually gave (n.p + d = 0): a unit normal
// pointing mostly up (y > 0.5); a v2 sends zeros until it has found one.
bool kinect_floor_known(const std::array<float, 4>& floor);
// The turn that makes a floor level: R n = (0, 1, 0), first a roll about
// the camera's z (taking out the normal's x), then a pitch about its x
// (taking out its z), so the optical axis keeps its heading (no yaw).
// Row-major. The identity for a floor that is not known.
std::array<float, 9> kinect_level_rotation(const std::array<float, 4>& floor);
// The camera's tilt from a floor normal, in degrees, for the log: pitch
// positive when it looks down, roll positive when the floor's normal leans
// to the camera's +x.
void kinect_floor_tilt(const std::array<float, 4>& floor, float& pitch_degrees, float& roll_degrees);

// The fit view's stretch: the v1's focal length over the v2's scaled so its
// 424 rows fit the v1's 240, f1 / (f2 * 240 / 424); about 1.378 for a
// typical v2 (focal length 366).
float kinect_v2_fit_scale(float v2_focal_length);
constexpr float kinect_v2_typical_depth_focal = 366.0f;
// The true distances (metres) that land inside the v1's 800..4000 mm under a
// stretch.
float kinect_depth_true_minimum(float z_scale);
float kinect_depth_true_maximum(float z_scale);

// A Kinect v2 always makes the depth (no switch). SFR_KINECT_V2_DEPTH_VIEW
// (developer tuning) chooses where the virtual v1 is:
// - fit (the default): at the v2, with depth stretched so the v2's whole
//   view fits the v1's image (KinectDepthView); no state, so one or two
//   players and relays are the game's own business. The stretch is
//   kinect_v2_fit_scale of the sensor's own focal length (its typical one
//   until the sensor gives it), or SFR_KINECT_V2_DEPTH_FIT_SCALE (1.0..2.0;
//   1.0 is the sensor view).
// - sensor: at the v2, unstretched (the v1's narrower view of it).
// - player: where a v1 would be for the player: the player's reference point
//   (latched, KinectDepthViewLatch) SFR_KINECT_V2_DEPTH_DISTANCE metres away
//   (default 2.5, 1.0..4.0) and, when the floor is known, the camera
//   SFR_KINECT_V2_DEPTH_HEIGHT metres above it (default 0.8, 0.2..2.0).
// - ideal: a real v1 standing where it should: level (the floor plane made
//   horizontal), SFR_KINECT_V2_DEPTH_DISTANCE in front of the players
//   (default 2.5), SFR_KINECT_V2_DEPTH_HEIGHT above the floor (default 1.0),
//   following the players' mean SpineBase slowly (KinectIdealState:
//   SFR_KINECT_V2_DEPTH_FOLLOW seconds, default 2.0, 0.2..20, outside a dead
//   band of SFR_KINECT_V2_DEPTH_DEADBAND metres, default 0.15, 0..1), with
//   no stretch: sizes, depths and movements are a v1's at that distance.
enum class KinectDepthViewMode : uint8_t { fit, sensor, player, ideal };
struct KinectDepthSettings {
    KinectDepthViewMode mode = KinectDepthViewMode::fit;
    bool player_view = false;  // mode == player
    float distance = 2.5f, height = 0.8f;  // height 1.0 by default for ideal
    float fit_scale = 0;       // set by SFR_KINECT_V2_DEPTH_FIT_SCALE; 0 from the sensor's lens
    float follow = 2.0f, deadband = 0.15f;  // ideal
    // The skeleton handed to the game, apart from the depth: fit's z stretch
    // for the skeleton, SFR_KINECT_V2_SKELETON_STRETCH (1.0..2.0; unset 1.0,
    // the sensor's real metres; "depth", stored as 0, the depth's k), and
    // the ideal camera's distance for the skeleton,
    // SFR_KINECT_V2_SKELETON_DISTANCE (1.0..4.0; 0, unset, the depth's).
    // The title never projects a skeleton into the depth image (it never
    // calls NuiTransformSkeletonToDepthImage), so the two need not agree;
    // its menu cursor is the 3D angle of shoulder to hand, which a z stretch
    // of k makes about 1/k as sensitive for a hand reaching forward.
    float skeleton_stretch = 1.0f, skeleton_distance = 0;
    static KinectDepthSettings from_environment();
    // The values from text as the environment gives them (null for unset).
    static KinectDepthSettings parse(const char* view, const char* distance, const char* height,
                                     const char* fit_scale = nullptr, const char* follow = nullptr,
                                     const char* deadband = nullptr, const char* skeleton_stretch = nullptr,
                                     const char* skeleton_distance = nullptr);
    const char* view_name() const;
};
// The launcher's choice (kinect_ideal_diagram.h) as the settings the game
// would read for it: parse() of the same text SFR_KINECT_V2_DEPTH_* would
// carry, so the clamps and defaults are the game's. The tuning is kept for
// every view (fit and sensor do not use it), so the ideal state a viewer keeps
// for the preview's diagram follows the launcher's sliders whatever is shown.
KinectDepthSettings kinect_depth_settings(const KinectDepthChoice& choice);

// The player view's placement, latched rather than following: the player's
// reference point (their skeleton centre, a v2's SpineBase) is taken when
// someone first becomes tracked -- the most central body, averaged over its
// first settle_frames frames (0.5 s), with the first frame's placement
// meanwhile -- and then kept while anyone is tracked, so that side steps and
// the hips going down and up stay in the image as movement. Once nobody has
// been tracked for release_seconds it is let go, and the next person latches
// it again. The sensor view is the v2's own place throughout.
class KinectDepthViewLatch {
public:
    static constexpr uint32_t settle_frames = 15;
    static constexpr double release_seconds = 1.0;
    explicit KinectDepthViewLatch(const KinectDepthSettings& settings) : settings_(settings) {}
    // The view for a frame of the sensor's own space, at seconds (any clock
    // that goes forward).
    const KinectDepthView& update(const KinectFrame& frame, double seconds);
    const KinectDepthView& view() const { return view_; }
    bool latched() const { return state_ == State::latched; }
    // Where the camera goes for this reference point and floor.
    KinectDepthView place(const std::array<float, 3>& reference, const std::array<float, 4>& floor) const;
private:
    enum class State { empty, settling, latched };
    KinectDepthSettings settings_;
    State state_ = State::empty;
    KinectDepthView view_;
    uint32_t tracking_id_ = 0, count_ = 0;
    std::array<double, 3> sum_{};
    double last_seen_ = 0;
};

// The ideal view's camera, stepped once per depth frame by a pure function
// of the previous state, the frame's time and the newest body frame's
// bodies and floor (kinect_ideal_step), so that a raw recording
// (kinect_record_raw.h) gives the same images again offline.
// - The floor plane is smoothed (kinect_ideal_floor_seconds; the first known
//   one taken as it is, an unknown one leaves it) and makes the turn that
//   levels the camera (kinect_level_rotation). No floor yet: no turn, and the
//   camera at the sensor's own height.
// - The reference is the mean SpineBase of all bodies given (one or two
//   players alike), in the levelled space; its x and z are followed by an
//   exponential of time constant follow seconds, only by what lies beyond the
//   dead band (per axis), so leaning, side steps and hops stay movements in
//   the image while a slow drift, or a player joining or leaving, glides in.
//   The first body after nobody (and after the floor first becomes known)
//   is taken at once; nobody holds the camera where it was. Height never
//   follows: the camera is height above the floor.
// - The camera: x on the reference, distance in front of it (z), height above
//   the floor, level, the v1's lens, no stretch.
constexpr double kinect_ideal_floor_seconds = 1.0;
// A Kinect v2 sends a floor plane whose normal is known (from its gravity)
// but whose distance is 0 until it has really found the floor, and for as
// long as it does not see it (FloorClipPlane's W): the normal then levels
// the camera, but its height above the floor is unknown, so the camera stays
// at the sensor's own height. A distance of at least
// kinect_floor_height_minimum (metres) is a height; the first one is taken at
// once, later ones are smoothed, and a plane without one leaves it.
constexpr float kinect_floor_height_minimum = 0.05f;
bool kinect_floor_height_known(const std::array<float, 4>& floor);
struct KinectIdealState {
    bool started = false;              // stepped at least once
    double seconds = 0;                // the last step's time
    bool floor_known = false;          // the normal: levelled
    bool height_known = false;         // and its distance: the camera's height above the floor
    std::array<float, 4> floor{};      // smoothed, the sensor's own space
    bool placed = false;               // a reference was taken
    bool had_bodies = false;           // the last step had someone
    std::array<float, 2> reference{};  // smoothed x, z, levelled space
    uint32_t bodies = 0;               // bodies in the last step
};
KinectIdealState kinect_ideal_step(const KinectIdealState& previous, const KinectDepthSettings& settings, double seconds,
                                   const KinectFrame& bodies);
KinectDepthView kinect_ideal_view(const KinectIdealState& state, const KinectDepthSettings& settings);
// The ideal view top-down (kinect_ideal_diagram.h): the virtual camera's x
// and z from kinect_ideal_view, and each body's SpineBase turned into the
// same levelled space (bodies as KinectDepthViewer::bodies keeps them; slot
// left -1 for the caller to fill).
KinectIdealSnapshot kinect_ideal_snapshot(const KinectFrame& bodies, const KinectIdealState& state,
                                          const KinectDepthSettings& settings);

// The virtual camera for every view, the one place the live sensor
// (kinect_v2_win32.cpp) and the offline re-synthesis (kinect_v2_resynth.h)
// both keep it, so the two cannot drift apart:
// - body_frame for every body frame, in the sensor's own space and before
//   any smoothing, at seconds on the body frames' clock (a v2's RelativeTime
//   in seconds): the player view's latch follows it, the ideal view keeps its
//   bodies (tracking id, body index, SpineBase) and floor for the next depth
//   frame. Returns the depth's view; the skeleton handed to the game goes
//   through skeleton_view().
// - depth_frame for every depth image about to be made, at seconds on the
//   depth frames' clock: the ideal view steps. Returns the view the image is
//   made through.
// fit, sensor and player are as they were; ideal is new. The ideal state is
// stepped in every view (only ideal makes its images through it), so the
// preview's diagram shows where Recompiled's camera would be whatever view
// is made.
class KinectDepthViewer {
public:
    explicit KinectDepthViewer(const KinectDepthSettings& settings);
    const KinectDepthSettings& settings() const { return settings_; }
    // Other settings from now on (the preview's view and the launcher's
    // tuning, KinectSensor::set_depth_settings). The ideal state is kept (a
    // new tuning applies from the next step), the player view's latch starts
    // again when its view or placement changes, and the fit stretch is the
    // setting's when it has one, otherwise the one already known.
    void set_settings(const KinectDepthSettings& settings);
    // The fit view's stretch (from the sensor's lens, or the setting).
    void set_fit_scale(float z_scale);
    float fit_scale() const { return fit_scale_; }
    const KinectDepthView& body_frame(const KinectFrame& frame, double seconds);
    const KinectDepthView& depth_frame(double seconds);
    const KinectDepthView& view() const { return view_; }
    // The view the skeleton handed to the game goes through: the depth's,
    // except for fit with its own skeleton stretch (the depth's origin, z
    // scaled by skeleton_stretch) and ideal with its own skeleton distance
    // (the same level camera moved along z so the reference is that far).
    KinectDepthView skeleton_view() const;
    // The newest body frame as the ideal view keeps it (bodies with their
    // tracking id, sensor index, SpineBase position and its state in
    // joint_states[0]; the floor), and its time.
    const KinectFrame& bodies() const { return bodies_; }
    double bodies_seconds() const { return bodies_seconds_; }
    bool latched() const { return latch_.latched(); }
    const KinectIdealState& ideal() const { return ideal_; }
    // Where the ideal view's camera is now (the view itself in ideal).
    KinectDepthView ideal_view() const { return kinect_ideal_view(ideal_, settings_); }
private:
    KinectDepthSettings settings_;
    float fit_scale_ = 1.0f;
    KinectDepthViewLatch latch_;
    KinectIdealState ideal_;
    KinectFrame bodies_;
    double bodies_seconds_ = 0;
    KinectDepthView view_;
};

// How a synthesized image came out.
struct KinectDepthStats {
    uint32_t valid = 0;    // pixels with a depth
    uint32_t players = 0;  // of those, a player's
    uint32_t filled = 0;   // single-pixel holes filled
};

// The v1 image from a v2 frame. points are the v2 depth pixels in camera
// space (x, y, z metres, three floats a pixel; a z that is not positive and
// finite is no point), body_index each pixel's body (0..5,
// kinect_v2_no_body for none; null for none at all), count the pixels.
// Each point is moved into the view and projected; the nearest wins its
// pixel. A pixel left empty between two that agree (both horizontal or both
// vertical neighbours, within 50 mm and the same player) takes their mean;
// anything else empty, or outside the v1's range, is no value. out is
// 320x240, 2 bytes a pixel in host order (KinectImage); its number is left
// alone.
KinectDepthStats kinect_v2_synthesize_depth(const float* points, const uint8_t* body_index, size_t count,
                                            const KinectDepthView& view, KinectImage& out);

// The view a made image went through, from its own fields (view_origin,
// view_z_scale, view_rotated, view_rotation).
KinectDepthView kinect_image_view(const KinectImage& image);
// The other way from kinect_v2_synthesize_depth: each pixel of a v1 depth
// image (2 bytes a pixel, the millimetres shifted up three) back to the
// point of the sensor's own camera space it shows. The pixel's centre
// through the v1's nominal lens at its depth, (x - 160, -(y - 120)) z / f,
// then out of the view (kinect_depth_view_unpoint). Three floats a pixel,
// zeros for a pixel without a value. A point is exact in depth and within
// half a pixel across (the synthesis rounds to the nearest pixel), so a
// colour sampled for it falls on the same pixel as its depth.
void kinect_v1_depth_points(const KinectImage& depth, const KinectDepthView& view, std::vector<float>& points);

// Camera-space points from v2 depth millimetres through a typical v2's lens
// (focal length 366 at 512x424, centre (256, 212); kinect_v2_depth_estimate
// the other way), for when the sensor's mapper gives none.
void kinect_v2_depth_points_from_lens(const uint16_t* millimetres, uint32_t width, uint32_t height,
                                      std::vector<float>& points);
// Camera-space points from v2 depth millimetres and a ray per pixel (x / z,
// y / z, camera y up; ICoordinateMapper::GetDepthFrameToCameraSpaceTable's
// table): (rx z, ry z, z). For the offline re-synthesis of a raw recording.
void kinect_v2_depth_points_from_rays(const uint16_t* millimetres, const float* rays, size_t count,
                                      std::vector<float>& points);
// The rays of a lens without distortion (focal lengths and centre in depth
// pixels): ((x - cx) / fx, -(y - cy) / fy).
void kinect_v2_depth_rays_from_lens(float focal_x, float focal_y, float centre_x, float centre_y, uint32_t width,
                                    uint32_t height, std::vector<float>& rays);
// A v2's tracking id as the player slots key on it (32 bits: its low half,
// or the high half with bit 0 set when the low half is 0).
uint32_t kinect_v2_short_tracking_id(uint64_t id);

// Recording the images made (SFR_KINECT_RECORD_DEPTH): kinect_record_depth.h,
// with the view each was made through (KinectImage::view_origin, view_z_scale).

}
