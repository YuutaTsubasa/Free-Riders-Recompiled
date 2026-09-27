#pragma once
#include "nui_skeleton.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
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
// Runtime 1.8 installed. Other platforms have no skeleton tracker for it;
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
    std::vector<KinectBody> bodies;  // at most two: the sensor tracks two fully
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
