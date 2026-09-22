#include "guest_memory.h"
#include "nui_device_status.h"
#include "nui_skeleton.h"
#include <bit>
#include <cmath>
#include <iostream>
#include <stdexcept>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static float load_float(sfr::GuestMemory& memory, uint64_t address) {
    return std::bit_cast<float>(memory.load<uint32_t>(address));
}

int main() {
    try {
        constexpr uint32_t frame = 0x10000;
        sfr::GuestMemory memory;
        memory.map(frame, 0x1000);
        for (uint32_t i = 0; i < 0x1000; ++i) memory.store<uint8_t>(frame + i, 0xa5);

        sfr::NuiSkeletonEmulation skeleton;
        skeleton.write(memory, frame, 7, 1234);
        require(memory.load<uint64_t>(frame) == 1234 && memory.load<uint32_t>(frame + 8) == 7,
                "timestamp and frame number lead the frame");
        require(load_float(memory, frame + 16 + 4) == 1.0f, "floor plane faces up");
        const uint32_t data = frame + sfr::nui_skeleton_data_offset;
        require(memory.load<uint32_t>(data) == sfr::nui_tracked, "first skeleton is tracked");
        for (uint32_t slot = 1; slot < 6; ++slot)
            require(memory.load<uint32_t>(data + slot * sfr::nui_skeleton_data_size) == 0,
                    "other skeletons are not tracked");
        for (uint32_t j = 0; j < sfr::nui_joint_count; ++j) {
            require(memory.load<uint32_t>(data + 352 + j * 4) == sfr::nui_tracked, "every joint is tracked");
            require(load_float(memory, data + 32 + j * 16 + 12) == 1.0f, "joint positions are points");
        }
        const uint32_t right_hand = data + 32 + sfr::nui_joint::hand_right * 16;
        require(load_float(memory, right_hand) > 0 && load_float(memory, right_hand + 8) > 2.0f,
                "right hand rests at the player's right side, in front of the sensor");
        require(memory.load<uint8_t>(frame + sfr::nui_skeleton_frame_size) == 0xa5,
                "nothing is written past the frame");

        // The first stick input raises the right hand for a second (so the
        // title starts its cursor), then centres it; the stick then moves it.
        sfr::GamepadState pad;
        pad.thumb_ry = 32767;
        const auto before = skeleton.hand(true);
        skeleton.update(pad);
        require(skeleton.hand(true)[1] > 0.4f, "first input raises the hand above the shoulder");
        for (int i = 1; i < 30; ++i) skeleton.update(pad);
        const auto centred = skeleton.hand(true);
        require(centred[1] > 0.1f && centred[1] < 0.3f && centred[2] < before[2], "raised hand is centred");
        for (int i = 0; i < 10; ++i) skeleton.update(pad);
        require(skeleton.hand(true)[1] > centred[1] + 0.2f, "stick then moves the hand");
        pad = {};
        pad.buttons = sfr::gamepad_button::back;
        skeleton.update(pad);
        require(skeleton.hand(true) == before, "BACK returns the hand to rest");

        // Identification raises the hand by itself and parks it up and to
        // the left, where the cursor covers no button; the stick moves it on.
        sfr::NuiSkeletonEmulation joined;
        joined.identify();
        joined.update({});
        require(joined.hand(true)[1] > 0.4f, "identified player raises the hand");
        for (int i = 1; i < 30; ++i) joined.update({});
        const auto parked = joined.hand(true);
        require(parked[0] < centred[0] - 0.15f && parked[1] > centred[1] + 0.25f, "raised hand is parked top-left");
        joined.update({});
        require(joined.hand(true) == parked, "parked hand stays without input");
        // A race reads the body's pose, so the parked cursor hand comes down.
        joined.update({}, true);
        require(joined.hand(true) == before, "a race rests the hands");
        // In a race the right stick reaches an arm out to its side, or both up.
        sfr::GamepadState reach{};
        reach.thumb_rx = -32768;
        joined.update(reach, true);
        require(joined.hand(false)[0] < -0.6f && joined.hand(false)[1] > 0.3f && joined.hand(true) == before,
                "stick left holds the left arm out");
        reach.thumb_rx = 32767;
        joined.update(reach, true);
        require(joined.hand(true)[0] > 0.6f && joined.hand(true)[1] > 0.3f, "stick right holds the right arm out");
        reach.thumb_rx = 0;
        reach.thumb_ry = 32767;
        joined.update(reach, true);
        require(joined.hand(true)[1] > 0.7f && joined.hand(false)[1] > 0.7f, "stick up raises both arms");
        joined.update({}, true);
        require(joined.hand(true) == before, "released, the arms rest again");
        pad = {};
        pad.thumb_rx = 32767;
        joined.update(pad);
        require(joined.hand(true)[0] > parked[0], "stick moves the parked hand");
        joined.identify();
        joined.update({});
        require(joined.hand(true)[1] < 0.6f, "identifying again does not raise an engaged hand again");

        memory.map(0x20000, 0x1000);
        sfr::write_nui_device_status(memory, 0x20000, sfr::emulated_nui_device_status);
        require(memory.load<uint32_t>(0x2000c) == 3 && memory.load<uint32_t>(0x20008) == 0,
                "emulated sensor reports connected and ready at +12 only");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "NUI skeleton checks passed\n";
}
