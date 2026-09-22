#include "nui_skeleton.h"
#include "guest_memory.h"
#include <algorithm>
#include <bit>
#include <cmath>

namespace sfr {
namespace {
using Vector = std::array<float, 3>;

float axis(int16_t value) {
    // XInput's recommended thumbstick dead zone.
    constexpr float dead = 7849.0f;
    const float v = float(value);
    if (std::fabs(v) < dead) return 0.0f;
    return (v - std::copysign(dead, v)) / (32767.0f - dead);
}

// Hands rest at the sides. The title starts tracking a menu cursor only
// after a hand is raised past about shoulder height; afterwards the right
// hand at (0.175, 0.2) puts the cursor at the screen centre and the parked
// hand puts it near the top-left corner (about 60, 50), clear of buttons.
Vector rest_position(bool right) { return {right ? 0.25f : -0.25f, -0.15f, 2.45f}; }
constexpr Vector raised_hand{0.175f, 0.45f, 2.25f}, centred_hand{0.175f, 0.2f, 2.25f};
constexpr Vector parked_hand{-0.046f, 0.526f, 2.25f};

void move(Vector& hand, float x, float y, bool right, bool push, bool rest) {
    if (rest) { hand = rest_position(right); return; }
    // Metres per 30 Hz frame at full deflection. The menu cursor moves about
    // 2600 px per metre sideways and 950 px per metre vertically, so both
    // axes cross the screen at roughly 900 px per second.
    constexpr float speed_x = 0.35f / 30.0f, speed_y = 1.0f / 30.0f;
    const float side = right ? 1.0f : -1.0f;
    hand[0] = std::clamp(hand[0] + x * speed_x, right ? -0.2f : -0.7f, right ? 0.7f : 0.2f);
    hand[1] = std::clamp(hand[1] + y * speed_y, -0.3f, 0.7f);
    hand[2] = push ? 2.1f : (hand[0] * side > 0.3f || hand[1] > -0.1f ? 2.25f : 2.45f);
}

Vector lerp(const Vector& a, const Vector& b, float t) {
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}

void store_float(GuestMemory& memory, uint64_t address, float value) {
    memory.store<uint32_t>(address, std::bit_cast<uint32_t>(value));
}

void store_vector(GuestMemory& memory, uint64_t address, const Vector& v, float w) {
    for (uint32_t i = 0; i < 3; ++i) store_float(memory, address + i * 4, v[i]);
    store_float(memory, address + 12, w);
}
}

void NuiSkeletonEmulation::update(const GamepadState& pad, bool racing) {
    const float rx = axis(pad.thumb_rx), ry = axis(pad.thumb_ry);
    if (racing) {
        engage_ = 0;
        left_ = rest_position(false);
        right_ = rest_position(true);
        if (ry > 0.5f) {
            left_ = {-0.2f, 0.8f, 2.4f};
            right_ = {0.2f, 0.8f, 2.4f};
        } else if (rx < -0.3f) {
            left_ = {-0.18f - 0.55f * -rx, 0.38f, 2.45f};  // out to the side at shoulder height
        } else if (rx > 0.3f) {
            right_ = {0.18f + 0.55f * rx, 0.38f, 2.45f};
        }
        return;
    }
    const bool rest = (pad.buttons & gamepad_button::back) != 0;
    if (rest) engage_ = 0;
    // The first right-stick input raises the hand (one second) and then
    // centres it, so the cursor appears at the centre of the screen.
    if (!rest && engage_ == 0 && (rx != 0.0f || ry != 0.0f)) { engage_ = 1; park_ = false; }
    if (engage_ >= 1 && engage_ <= 30) {
        right_ = raised_hand;
        if (++engage_ > 30) right_ = park_ ? parked_hand : centred_hand;
        return;
    }
    // The shoulders push the hands towards the sensor.
    move(right_, rx, ry, true, (pad.buttons & gamepad_button::right_shoulder) != 0, rest);
    move(left_, axis(pad.thumb_lx), axis(pad.thumb_ly), false, (pad.buttons & gamepad_button::left_shoulder) != 0, rest);
}

void NuiSkeletonEmulation::write(GuestMemory& memory, uint32_t address, uint32_t frame_number,
                                 uint64_t timestamp_ms) const {
    memory.check_write(address, nui_skeleton_frame_size);
    for (uint32_t offset = 0; offset < nui_skeleton_frame_size; offset += 4)
        memory.store<uint32_t>(uint64_t(address) + offset, 0);
    memory.store<uint64_t>(address, timestamp_ms);
    memory.store<uint32_t>(uint64_t(address) + 8, frame_number);
    // Floor 1 m below the sensor; gravity straight down.
    store_vector(memory, uint64_t(address) + 16, {0.0f, 1.0f, 0.0f}, 1.0f);
    store_vector(memory, uint64_t(address) + 32, {0.0f, 1.0f, 0.0f}, 0.0f);

    std::array<Vector, nui_joint_count> joints{};
    using namespace nui_joint;
    constexpr float z = 2.5f;
    joints[hip_center] = {0.0f, -0.05f, z};
    joints[spine] = {0.0f, 0.1f, z};
    joints[shoulder_center] = {0.0f, 0.42f, z};
    joints[head] = {0.0f, 0.62f, z};
    joints[shoulder_left] = {-0.18f, 0.38f, z};
    joints[shoulder_right] = {0.18f, 0.38f, z};
    for (const bool right : {false, true}) {
        const Vector& shoulder = joints[right ? shoulder_right : shoulder_left];
        const Vector& hand = right ? right_ : left_;
        Vector elbow = lerp(shoulder, hand, 0.5f);
        elbow[1] -= 0.05f;
        joints[right ? elbow_right : elbow_left] = elbow;
        joints[right ? wrist_right : wrist_left] = lerp(shoulder, hand, 0.92f);
        joints[right ? hand_right : hand_left] = hand;
    }
    joints[hip_left] = {-0.1f, -0.1f, z};
    joints[hip_right] = {0.1f, -0.1f, z};
    joints[knee_left] = {-0.11f, -0.52f, z};
    joints[knee_right] = {0.11f, -0.52f, z};
    joints[ankle_left] = {-0.12f, -0.92f, z};
    joints[ankle_right] = {0.12f, -0.92f, z};
    joints[foot_left] = {-0.12f, -0.97f, z - 0.08f};
    joints[foot_right] = {0.12f, -0.97f, z - 0.08f};

    const uint64_t data = uint64_t(address) + nui_skeleton_data_offset;  // slot 0 only
    memory.store<uint32_t>(data, nui_tracked);
    memory.store<uint32_t>(data + 4, 1);  // tracking id
    // Until identified (-1) the title runs NuiIdentityIdentify on it before
    // it may join; afterwards the guest result.
    memory.store<uint32_t>(data + 8, enrollment_);
    store_vector(memory, data + 16, joints[hip_center], 1.0f);
    for (uint32_t j = 0; j < nui_joint_count; ++j) {
        store_vector(memory, data + 32 + j * 16, joints[j], 1.0f);
        memory.store<uint32_t>(data + 352 + j * 4, nui_tracked);
    }
}
}
