#pragma once
#include "nui_skeleton.h"

#include <array>
#include <cstdint>

namespace sfr {
class GuestMemory;

// NuiTransformSmooth, the NUI library's skeleton filter, as the title links
// it (827701A0, which runs 82779700 for each skeleton) and as the Kinect
// for Windows SDK 1.8 exports it from Kinect10.dll: Holt's double
// exponential smoothing of each joint, with a jitter radius and a cap on how
// far the prediction may lead the measurement. Read from the title's code:
//
// - Each of the six skeleton slots keeps a history of 20 joints (raw,
//   filtered, trend, frames seen). A slot whose skeleton is not TRACKED (2)
//   has its history cleared; only TRACKED skeletons are filtered. Here a
//   slot's history is also cleared when its tracking id changes (another
//   person in the slot), which the console's library does not check.
// - A joint at exactly (0, 0, 0) starts its history again and is passed on
//   as it is; being that history's first frame, the next one is its mean
//   with the origin (held back to the deviation radius), as in the
//   library's code. A joint's tracking state matters only for its radii: inferred
//   (1) joints get twice the jitter and deviation radius; not tracked (0)
//   joints with a position are filtered like tracked ones.
// - The jitter radius is at least 0.0001 m. A skeleton's position (its
//   centre) is not filtered; each joint's w becomes 1.
// - First frame: as measured, no trend. Second: the mean of the two
//   measurements, trend corrected towards their difference. Then: inside
//   the jitter radius the measurement is pulled towards the last output in
//   proportion to how far it moved; filtered = (1 - smoothing) measurement
//   + smoothing (last filtered + last trend); trend = correction (filtered -
//   last filtered) + (1 - correction) last trend; output = filtered +
//   prediction trend, drawn back to max deviation radius from the
//   measurement.
struct NuiSmoothParameters {
    float smoothing = 0.5f, correction = 0.5f, prediction = 0.5f;
    float jitter_radius = 0.05f, max_deviation_radius = 0.04f;  // metres
};
struct NuiSmoothJoint {
    std::array<float, 3> raw{}, filtered{}, trend{};
    uint32_t frames = 0;  // 0, 1, then 2 for good
};
using NuiSmoothSkeleton = std::array<NuiSmoothJoint, nui_joint_count>;

// One joint of one frame: returns the output and advances its history.
std::array<float, 3> nui_smooth_joint(NuiSmoothJoint& history, const std::array<float, 3>& measured, uint32_t state,
                                      const NuiSmoothParameters& parameters);
// A tracked skeleton's joints in place.
void nui_smooth_skeleton(NuiSmoothSkeleton& history, SkeletonJoints& joints,
                         const std::array<uint32_t, nui_joint_count>& states, const NuiSmoothParameters& parameters);

// The six slots of a skeleton frame, each with its own history.
class NuiFrameSmoother {
public:
    struct Slot {
        uint32_t tracking_state = 0, tracking_id = 0;
        SkeletonJoints joints{};
        std::array<uint32_t, nui_joint_count> states{};
    };
    // Smooths every TRACKED slot in place; clears the others' history.
    void smooth(std::array<Slot, 6>& slots, const NuiSmoothParameters& parameters);
    void reset();
    // The frame number nui_transform_smooth_guest smoothed last, and what
    // that gave.
    bool has_last = false;
    uint32_t last_frame = 0;
    std::array<Slot, 6> last{};
private:
    std::array<NuiSmoothSkeleton, 6> history_{};
    std::array<uint32_t, 6> ids_{};
};

// The parameters at a guest NUI_TRANSFORM_SMOOTH_PARAMETERS (five
// big-endian floats), or the library's defaults for address 0.
NuiSmoothParameters nui_smooth_parameters(GuestMemory& memory, uint32_t address);
// NuiTransformSmooth on the title's big-endian NUI_SKELETON_FRAME at frame
// (nui_skeleton.h's layout): S_OK (0), or E_POINTER for no frame. A frame
// with the same frame number as the last one smoothed (dwFrameNumber, +8;
// the skeleton hook hands the title its last frame again when the sensor
// had nothing newer) is not filtered a second time: each slot whose tracking
// id is unchanged gets that frame's output again.
uint32_t nui_transform_smooth_guest(GuestMemory& memory, uint32_t frame, uint32_t parameters, NuiFrameSmoother& smoother);

}
