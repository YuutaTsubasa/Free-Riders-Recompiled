#pragma once
#include "nui_skeleton.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace sfr {

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
    std::array<float, 3> position{};           // the skeleton's centre
    SkeletonJoints joints{};                   // nui_joint order
    std::array<uint32_t, nui_joint_count> joint_states{};  // 0 not, 1 inferred, 2 tracked
};

struct KinectFrame {
    uint64_t number = 0;  // counts from one; 0 means nothing has arrived
    // The floor as the sensor sees it (ax + by + cz + d = 0) and the normal
    // to gravity (straight up); all zero when it could not tell.
    std::array<float, 4> floor_plane{};
    std::array<float, 3> gravity{};
    std::vector<KinectBody> bodies;  // the fully tracked ones: two on a v1, up to six on a v2
};

class KinectSensor {
public:
    virtual ~KinectSensor();
    // The newest frame, if one has arrived since the last call. False leaves
    // the frame alone.
    virtual bool next(KinectFrame& frame) = 0;
    // The first sensor the host has, started with skeleton tracking. Null
    // (with the reason in why) when there is no runtime or no sensor.
    static std::unique_ptr<KinectSensor> open(std::string* why = nullptr);
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
}
struct KinectV2Joint {
    std::array<float, 3> position{};  // camera space, metres, as v1's
    uint32_t state = 0;               // 0 not tracked, 1 inferred, 2 tracked
};
// A v2 body as the v1 skeleton the title reads.
void kinect_v2_body(const std::array<KinectV2Joint, kinect_v2_joint_count>& joints, uint32_t tracking_id,
                    KinectBody& body);

// Why no sensor opened, from why each kind failed ("no-runtime",
// "no-sensor", ...): a runtime that is installed says more than one that is
// not, so it is its reason that counts; with neither, "no-runtime".
std::string kinect_open_failure(const std::string& v1, const std::string& v2);

// Where the sensor stands around the player, seen from above with the
// screen ahead. The title was made for a sensor at the screen, but a board is
// ridden side-on, so a sensor at the side the chest faces sees the whole
// body instead of its profile; in a room that is wide but shallow it is also
// the only place far enough away. Straight to one side, an arm reaching
// behind the back is hidden by the body; a sensor at a diagonal still sees
// the chest and sees round to that arm. Whatever the placement, the skeleton
// the title reads is turned to look as if the sensor stood at the screen
// (KinectPlacementTransform).
enum class KinectPlacement : uint8_t {
    front, front_right, right, behind_right, behind, behind_left, left, front_left
};
// SFR_KINECT_PLACEMENT: "front" (or nothing), "front-right", "right",
// "behind-right", "behind", "behind-left", "left" or "front-left".
KinectPlacement kinect_placement_from(const char* text);
const char* kinect_placement_name(KinectPlacement placement);
// Clockwise from the screen, seen from above: 0 in front, 90 on the
// player's right, 180 behind, 270 on the left.
float kinect_placement_degrees(KinectPlacement placement);

// Turns bodies seen from elsewhere into the front sensor's camera space.
// Each body is placed where the emulated player stands (pose_distance in
// front of the sensor), measured from where it was when the sensor first
// found it: that point stays fixed, so stepping and leaning still move the
// body as they would have in front of a sensor at the screen. The sensor is
// taken to see the side the player's chest faces (or a profile, in front
// and behind), as its skeleton tracker assumes a body facing it.
class KinectPlacementTransform {
public:
    explicit KinectPlacementTransform(KinectPlacement placement = KinectPlacement::front) : placement_(placement) {}
    KinectPlacement placement() const { return placement_; }
    // In place, for every body of the frame. A front sensor's frame is left
    // exactly as it came; the others lose the sensor's floor plane, which
    // no longer describes the turned space.
    void apply(KinectFrame& frame);
private:
    KinectPlacement placement_;
    std::vector<std::pair<uint32_t, std::array<float, 3>>> anchors_;  // tracking id, first position
};

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
private:
    std::array<uint32_t, players> tracking_{};  // sensor ids; 0 is free
};

}
