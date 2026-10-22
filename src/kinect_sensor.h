#pragma once
#include "kinect_ideal_diagram.h"
#include "nui_skeleton.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

namespace sfr {

struct KinectDepthSettings;  // kinect_v2_depth.h

// A real Kinect standing in as itself. The title was written against the
// Xbox 360's NUI library, and the Kinect for Windows runtime (SDK 1.8,
// Kinect10.dll) is that same library brought to the PC: its skeleton frame
// carries the same six skeletons of twenty joints, in the same camera space
// (metres, +x towards the player's right, +z away from the sensor). So what
// the sensor tracks is handed to the title as it is, and the title's own
// gesture detectors read a real body instead of the pad.
//
// SFR_CAMERA=kinect asks for it. A Kinect for Xbox 360 (with its USB power
// adapter) or a Kinect for Windows v1 both work, with the Kinect for Windows
// SDK 1.8 installed. So does the newer Kinect v2 (Xbox One or Kinect for
// Windows v2, with its adapter and SDK 2.0, Kinect20.dll): the same camera
// space, and 25 joints of which the 20 the title knows are taken (see
// kinect_v2_body). The first sensor found is used, the older kind first.
// Other platforms have no skeleton tracker for either;
// on Linux the kernel's gspca_kinect driver shows the sensor's colour camera
// as a webcam, which SFR_CAMERA=motion can read instead.

// One body the sensor is tracking fully.
struct KinectBody {
    uint32_t tracking_id = 0;                  // the sensor's, stable while tracked
    // Where the sensor put it among its skeletons; its pixels in the depth
    // image carry this plus one as their player index.
    uint32_t sensor_index = 0;
    std::array<float, 3> position{};           // the skeleton's centre
    SkeletonJoints joints{};                   // nui_joint order
    std::array<uint32_t, nui_joint_count> joint_states{};  // 0 not, 1 inferred, 2 tracked
    // A Kinect v2's reading of each hand (left, right): 0 unknown, 1 not
    // tracked, 2 open, 3 closed (a fist), 4 lasso. A v1 leaves them 0.
    std::array<uint8_t, 2> hand_states{};
    // A Kinect v2's fingertips and thumbs (left, right), which give which
    // way a palm faces; all zero from a v1 or when not seen.
    std::array<std::array<float, 3>, 2> hand_tips{}, thumbs{};
    // Which edges of the sensor's picture cut this body off (kinect_edge
    // bits; 0 none). Only a Kinect v2 says (IBody::ClippedEdges); a v1 leaves
    // it 0, which there means unknown, not "not cut". Display and log only:
    // no detector or the game reads it.
    uint8_t clipped_edges = 0;
};

// The bits of KinectBody::clipped_edges, the SDK's FrameEdges. Left and
// right are the picture's own, as the preview draws it (the sensor's raw
// image, not mirrored): the picture's left shows the sensor's left, which is
// the PLAYER's right while the player faces the sensor.
namespace kinect_edge {
constexpr uint8_t right = 0x1, left = 0x2, top = 0x4, bottom = 0x8, all = 0xF;
}
// "top+left" (picture edges, top, bottom, left, right order), "none" for 0.
std::string kinect_edges_text(uint8_t edges);
// What the player should do about clipped edges: bits of kinect_edge_move.
// A cut on the picture's left or right is cured by moving the other way
// round from the player's view: the picture's left edge is the player's
// right side (mirror), so the player steps to the player's own left.
namespace kinect_edge_move {
constexpr uint8_t step_back = 0x1, player_left = 0x2, player_right = 0x4, tilt_up = 0x8, tilt_down = 0x10;
}
uint8_t kinect_edges_move(uint8_t edges);

struct KinectFrame {
    uint64_t number = 0;  // counts from one; 0 means nothing has arrived
    // Set by the acquisition worker, never by a consumer reading its cache.
    std::chrono::steady_clock::time_point captured_at{};
    // The floor as the sensor sees it (ax + by + cz + d = 0) and the normal
    // to gravity (straight up); all zero when it could not tell.
    std::array<float, 4> floor_plane{};
    std::array<float, 3> gravity{};
    std::vector<KinectBody> bodies;  // the fully tracked ones: two on a v1, up to six on a v2
};

// One image from the sensor's cameras, rows packed, in host byte order:
// depth_and_player is 16 bits a pixel (the depth in millimetres shifted up
// three, the player index 1..6 in the low three bits, as the console's NUI
// has it), colour 32 bits a pixel (B, G, R, unused).
enum class KinectImageKind : uint8_t { depth_and_player = 0, colour = 1 };
struct KinectImage {
    uint64_t number = 0;  // counts from one; 0 means nothing has arrived
    uint32_t width = 0, height = 0, bytes_per_pixel = 0;
    std::vector<uint8_t> pixels;
    // For a recording (kinect_record_depth.h): when the backend had it (the
    // host's steady clock, kinect_record_now_us), the SDK's own timestamp
    // (a v1's liTimeStamp in ms, a v2's RelativeTime in 100 ns; 0 unknown)
    // and frame number (a v1's dwFrameNumber; 0 for a v2), and for a depth
    // image a Kinect v2 made for the game, the virtual v1 it was made
    // through (KinectDepthView; a v1's own image leaves these as they are).
    int64_t steady_us = 0, sdk_time = 0;
    uint64_t sdk_frame = 0;
    std::array<float, 3> view_origin{};
    float view_z_scale = 1.0f;
    // And its turn (KinectDepthView::rotated, rotation), so the image can be
    // taken back into the sensor's space (kinect_v1_depth_points). Not
    // recorded.
    bool view_rotated = false;
    std::array<float, 9> view_rotation{1, 0, 0, 0, 1, 0, 0, 0, 1};
};

// Where a point of skeleton space falls in one of the sensor's images, in
// that image's pixels; not visible when it cannot be placed. Visible means
// placed, not inside the image: a point beyond its edge is still visible
// (within a sane distance), and whoever draws it clips to the image.
struct KinectImagePoint { float x = 0, y = 0; bool visible = false; };

// Where the picture itself lies inside the preview's image of one kind
// (colour 640x480, depth 320x240), in those pixels. A picture of another
// shape (a Kinect v2's) is fitted with black bars around it, and nothing is
// drawn over the bars: skeletons are clipped at this rectangle.
struct KinectImageRect {
    float x = 0, y = 0, width = 0, height = 0;
    bool contains(float px, float py) const {
        return px >= x && px < x + width && py >= y && py < y + height;
    }
};

class KinectSensor {
public:
    virtual ~KinectSensor();
    // The newest image of one camera, if there is one newer than
    // image.number. A Kinect v1 opens its cameras
    // (and hands them to the game too: has_depth_stream). A Kinect v2 opens
    // its cameras on the first call, for the preview window only, at the
    // v1's sizes (kinect_v2_image.h), and closes them again when nobody has
    // asked for a few seconds; the game reads game_image instead. Others
    // have none.
    virtual bool image(KinectImageKind, KinectImage&) { return false; }
    // The newest image of one camera for the game's own image streams
    // (NuiImageStreamGetNextFrame), newer than image.number. A v1's are its
    // image()s. A v2 makes a v1's depth and
    // player index image of its own (kinect_v2_depth.h); its preview keeps
    // image().
    virtual bool game_image(KinectImageKind kind, KinectImage& image) { return this->image(kind, image); }
    // Why the game's depth stream is not there (a v1's depth stream or a v2's
    // depth and body index readers would not open), or empty when it is. The
    // race reads the title's own depth detectors and nothing stands in for a
    // missing depth: the game just gets no depth, as on a console whose
    // sensor has none.
    virtual std::string depth_failure() const { return {}; }
    // Where a skeleton point falls in this sensor's image of one kind (as
    // image() gives it), when the sensor places points itself: true with the
    // point, which may be not visible. False when it does not, and the
    // preview uses a v1's lens (kinect_depth_point, kinect_colour_point).
    virtual bool image_point(KinectImageKind, const std::array<float, 3>&, KinectImagePoint&) { return false; }
    // Where the picture lies inside the preview's image of one kind. False
    // (the default) means it fills the whole image, as a Kinect v1's does.
    virtual bool picture_rect(KinectImageKind, KinectImageRect&) { return false; }
    // What the preview shows of a sensor whose pictures differ in shape from
    // a v1's (a Kinect v2): true, the default, the view both cameras have;
    // false, each camera's whole picture (the window's "Original" box).
    // Changes images, picture_rect and image_point from then on; never what
    // the game or a recording gets. Others ignore it.
    virtual void set_preview_common_view(bool common) { (void)common; }
    // A Kinect v2's depth view and its tuning from now on, instead of the
    // SFR_KINECT_V2_DEPTH_* environment it starts with (kinect_v2_depth.h):
    // what the launcher's preview shows. Applied on the sensor's own thread
    // at its next frame; the game never calls it. Others ignore it.
    virtual void set_depth_settings(const KinectDepthSettings& settings) { (void)settings; }
    // Where the Recompiled (ideal) view's virtual v1 stands and where the
    // players are, as the newest depth image was made (a Kinect v2 keeps it
    // in every view: KinectDepthViewer). False, leaving it alone, when the
    // sensor makes no depth.
    virtual bool ideal_snapshot(KinectIdealSnapshot& snapshot) { (void)snapshot; return false; }
    // Where points of the sensor's own camera space (x, y, z metres, three
    // floats each; a z that is not positive is no point) land in the
    // preview's colour image (image(colour), its current view): x, y pairs
    // in its pixels, NaN for a point it cannot place or none. A Kinect v2
    // through its coordinate mapper (the typical lens without it); false,
    // leaving positions alone, for a sensor that cannot.
    virtual bool colour_image_positions(const std::vector<float>& points, std::vector<float>& positions) {
        (void)points; (void)positions;
        return false;
    }
    // Successfully opened streams the game reads (NuiImageStreamOpen),
    // rather than requested configuration. A v1's, and a v2's
    // (its depth and body index readers opened).
    virtual bool has_depth_stream() const { return false; }
    virtual bool has_colour_stream() const { return false; }
    // The sensor's tilt motor, in degrees above level (a Kinect v1 turns
    // from -27 to 27). False when the sensor has no motor or will not say.
    virtual bool elevation(int& degrees) { (void)degrees; return false; }
    // Turns the motor; the SDK asks for no more than a turn a second.
    virtual bool set_elevation(int degrees) { (void)degrees; return false; }
    // The newest frame, if one has arrived since the last call. False leaves
    // the frame alone.
    virtual bool next(KinectFrame& frame) = 0;
    // The first sensor the host has, started with skeleton tracking. Null
    // (with the reason in why) when there is no runtime or no sensor.
    static std::unique_ptr<KinectSensor> open(std::string* why = nullptr);
    // One kind only: 1 a Kinect v1 (SDK 1.8), 2 a Kinect v2 (SDK 2.0). Both
    // may be opened in one process (sfr_kinect_probe --both), each on its own
    // runtime (whether the two get on has not been tried on hardware); a
    // failure to open one leaves the other alone.
    static std::unique_ptr<KinectSensor> open_model(int model, std::string* why = nullptr);
    // What the launcher's camera choice asks for (SFR_KINECT_MODEL): 2 opens a
    // Kinect v2 only; anything else is the Xbox 360 Kinect, and when no v1
    // opens but a v2 does the v2 is used all the same (one log line says so),
    // so an old settings.ini with a v2 keeps working.
    static std::unique_ptr<KinectSensor> open_selected(int model, std::string* why = nullptr);
    // Whether this build can talk to a sensor at all.
    static bool supported();
    // "Kinect v1" or "Kinect v2", for the log.
    virtual const char* model() const = 0;
};

// Kinect v2's joints, in its JointType order: the first twenty are the v1
// joints in the same order, except that v2 puts its neck (2) above the
// shoulders and adds a spine point between them (20, SpineShoulder), which
// is where v1's shoulder centre sits. 21..24 are the fingertips and thumbs.
constexpr uint32_t kinect_v2_joint_count = 25;
namespace kinect_v2_joint {
constexpr uint32_t spine_base = 0, spine_mid = 1, neck = 2, spine_shoulder = 20;
constexpr uint32_t hand_tip_left = 21, thumb_left = 22, hand_tip_right = 23, thumb_right = 24;
}
struct KinectV2Joint {
    std::array<float, 3> position{};  // camera space, metres, as v1's
    uint32_t state = 0;               // 0 not tracked, 1 inferred, 2 tracked
};
// A v2 body as the v1 skeleton the title reads.
void kinect_v2_body(const std::array<KinectV2Joint, kinect_v2_joint_count>& joints, uint32_t tracking_id,
                    uint32_t sensor_index, KinectBody& body);

// Remove stale bodies after a brief acquisition gap. The source number and
// capture time stay unchanged; true means tracking was cleared this call.
bool kinect_expire_frame(KinectFrame& frame, std::chrono::steady_clock::time_point now);
// Enrollment only lasts while its real body remains tracked.
void kinect_prune_identities(const KinectFrame& frame, std::unordered_map<uint32_t, uint32_t>& identities);

// Why no sensor opened, from why each kind failed ("no-runtime",
// "no-sensor", ...): a runtime that is installed says more than one that is
// not, so it is its reason that counts; with neither, "no-runtime".
std::string kinect_open_failure(const std::string& v1, const std::string& v2);
// The model the launcher chose, from SFR_KINECT_MODEL: 2 for "kinect_v2", 1
// otherwise (unset included: an old launcher only knew "kinect").
int kinect_selected_model();
// SFR_KINECT_SMOOTH=1: the backends smooth their skeletons themselves (a
// v1 through its SDK's NuiTransformSmooth, a v2 through KinectJointSmoother,
// the same filter), for sfr_kinect_probe (--smooth) and tests. Off by
// default: the game smooths each real skeleton once, as the title asks
// (its NuiTransformSmooth, nui_smooth_hooks.cpp), and leaves that out when
// this is on, so a skeleton is never smoothed twice.
bool kinect_host_smoothing();

// Which of the tracked bodies play as the title's two Kinect players. The
// title follows a player by the slot and the tracking id it was identified
// under (1 for the first player, who is the signed-in profile, and 2), so a
// body keeps its slot for as long as the sensor keeps tracking it, and a
// body that steps in takes whichever slot is free. The first player's slot
// is filled first.
class KinectPlayerSlots {
public:
    static constexpr uint32_t players = 2;
    // The body in each slot for this frame, or null for an empty slot.
    std::array<const KinectBody*, players> assign(const KinectFrame& frame);
    // The title has space for two fully tracked bodies. Retain those stable
    // players without changing their original sensor/depth-image indices.
    void retain(KinectFrame& frame);
private:
    std::array<uint32_t, players> tracking_{};  // sensor ids; 0 is free
};

}
