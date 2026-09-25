#pragma once
#include "native_input.h"
#include <array>
#include <cstdint>

namespace sfr {
class GuestMemory;

// NUI_SKELETON_FRAME as the title's Kinect library copies it (2736 bytes,
// big-endian): timestamp (8), frame number, flags, floor clip plane and
// gravity normal (float4 at +16/+32), then six 448-byte NUI_SKELETON_DATA:
// tracking state, tracking id, enrollment and user index, position (+16),
// 20 joint positions (+32) and their tracking states (+352), quality (+432).
constexpr uint32_t nui_skeleton_frame_size = 2736;
constexpr uint32_t nui_skeleton_data_offset = 48, nui_skeleton_data_size = 448;
constexpr uint32_t nui_joint_count = 20;
constexpr uint32_t nui_tracked = 2;  // skeleton and joint "tracked" states

// Joint indices (NUI_SKELETON_POSITION_INDEX).
namespace nui_joint {
constexpr uint32_t hip_center = 0, spine = 1, shoulder_center = 2, head = 3;
constexpr uint32_t shoulder_left = 4, elbow_left = 5, wrist_left = 6, hand_left = 7;
constexpr uint32_t shoulder_right = 8, elbow_right = 9, wrist_right = 10, hand_right = 11;
constexpr uint32_t hip_left = 12, knee_left = 13, ankle_left = 14, foot_left = 15;
constexpr uint32_t hip_right = 16, knee_right = 17, ankle_right = 18, foot_right = 19;
}

// One player standing 2.5 m in front of the sensor (camera space, metres).
// The right hand moves like a cursor: once identified the player raises it
// and parks it in the top-left corner, since the title's hand-only dialogs
// (823EF348) run only while a cursor exists; after BACK has lowered it, the
// first right-stick input raises it to the centre of the screen. The stick
// sets its velocity and RB pushes it towards the sensor. The left stick and
// LB move the left hand the same way (without the raise).
class NuiSkeletonEmulation {
public:
    // Advances the hands by one 30 Hz frame of pad input. In a race the hands
    // rest at the player's sides (the cursor a menu needs is a raised arm,
    // and a parked hand would steer the board) unless the right stick
    // reaches: left or right holds that arm out to its side (for the rings
    // beside the board), up raises both.
    void update(const GamepadState& pad, bool racing = false);
    // Writes the complete frame for this player alone: the header, this
    // skeleton in slot 0 with tracking id 1, and no other slot tracked.
    void write(GuestMemory& memory, uint32_t address, uint32_t frame_number, uint64_t timestamp_ms) const;
    // The same in two parts, for a frame that carries more than one player:
    // the header (which clears every slot), then a slot each. A slot's
    // tracking id is what NuiIdentityIdentify is called with, so the players
    // need different ones.
    static void write_header(GuestMemory& memory, uint32_t address, uint32_t frame_number, uint64_t timestamp_ms);
    void write_slot(GuestMemory& memory, uint32_t address, uint32_t slot, uint32_t tracking_id) const;
    // A slot written from joints worked out elsewhere (a camera's pose), in
    // camera space and in the order nui_joint numbers them.
    void write_joints(GuestMemory& memory, uint32_t address, uint32_t slot, uint32_t tracking_id,
                      const std::array<std::array<float, 3>, nui_joint_count>& joints) const;
    std::array<float, 3> hand(bool right) const { return right ? right_ : left_; }
    // After NuiIdentityIdentify completes the skeleton carries that result
    // instead of "not yet identified": the enrollment of the signed-in
    // profile it was recognised as, or an unenrolled guest.
    static constexpr uint32_t unidentified = 0xFFFFFFFFu, guest = 0xFFFFFFFEu;
    void identify(uint32_t enrollment = guest) {
        enrollment_ = enrollment;
        if (engage_ == 0) { engage_ = 1; park_ = !hand_starts_centred(); }
    }
    // SFR_NUI_HAND_CENTRED=1 raises the hand to the middle of the screen
    // instead of parking it in the corner. A player wants it parked -- a
    // cursor sitting on a button presses it by waiting -- but an unattended
    // run has to steer it out of that corner with stick taps before it can
    // press anything, and the timing of that is what makes those runs flaky.
    static bool hand_starts_centred();
private:
    std::array<float, 3> right_{0.25f, -0.15f, 2.45f};
    uint32_t engage_ = 0;  // 0 at rest, 1-30 raising, 31 tracking
    bool park_ = false;    // the raise ends parked instead of centred
    std::array<float, 3> left_{-0.25f, -0.15f, 2.45f};
    uint32_t enrollment_ = unidentified;
};
}
