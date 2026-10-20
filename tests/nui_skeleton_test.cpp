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

        // A frame carrying two players: the header clears the slots, then
        // each player writes its own with a tracking id of its own.
        sfr::NuiSkeletonEmulation second;
        sfr::NuiSkeletonEmulation::write_header(memory, frame, 9, 4321);
        skeleton.write_slot(memory, frame, 0, 1);
        second.write_slot(memory, frame, 1, 2);
        require(memory.load<uint32_t>(data) == sfr::nui_tracked &&
                    memory.load<uint32_t>(data + 4) == 1, "the first player keeps slot 0 and tracking id 1");
        const uint32_t slot1 = data + sfr::nui_skeleton_data_size;
        require(memory.load<uint32_t>(slot1) == sfr::nui_tracked &&
                    memory.load<uint32_t>(slot1 + 4) == 2, "the second player is tracked in slot 1 as id 2");
        require(memory.load<uint32_t>(data + 12) == 0 && memory.load<uint32_t>(slot1 + 12) == 1,
                "each player carries its own user index");
        for (uint32_t slot = 2; slot < 6; ++slot)
            require(memory.load<uint32_t>(data + slot * sfr::nui_skeleton_data_size) == 0,
                    "no other slot is tracked with two players");
        require(memory.load<uint32_t>(frame + 8) == 9, "the header carries the frame number");

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
        // START in a story scene brings the parked cursor to the centre
        // (its SKIP button); a hand that is not up stays where it is.
        sfr::NuiSkeletonEmulation scene = joined;
        require(scene.centre_cursor() && scene.hand(true) == centred, "a raised hand moves to the centre");
        sfr::NuiSkeletonEmulation resting;
        require(!resting.centre_cursor() && resting.hand(true) == before, "a resting hand is not raised");
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

        // Issue #217: the menu cursor where a finger or the mouse points.
        // Cursor positions the title showed on the main menu for raised hands
        // (hand x, hand y, cursor x, cursor y).
        constexpr float measured[][4] = {{-0.03f, 0.65f, 216.8f, 0.0f},   {0.21f, 0.53f, 722.4f, 0.0f},
                                         {0.09f, 0.41f, 359.3f, 125.2f},  {0.33f, 0.29f, 1057.6f, 291.3f},
                                         {-0.03f, 0.17f, 173.7f, 457.5f}, {0.45f, 0.17f, 1202.3f, 457.5f},
                                         {-0.15f, 0.05f, 96.4f, 623.6f},  {0.21f, 0.05f, 698.1f, 623.6f}};
        for (const auto& m : measured) {
            const auto cursor = sfr::NuiSkeletonEmulation::cursor_for_hand(m[0], m[1]);
            require(std::fabs(cursor[0] - m[2]) < 25.0f && (m[3] == 0.0f || std::fabs(cursor[1] - m[3]) < 2.0f),
                    "the cursor model matches the title's cursor");
        }
        for (float x = 0.0f; x <= 1280.0f; x += 160.0f)
            for (float y = 0.0f; y <= 700.0f; y += 100.0f) {
                const auto hand = sfr::NuiSkeletonEmulation::hand_for_cursor(x, y);
                const auto back = sfr::NuiSkeletonEmulation::cursor_for_hand(hand[0], hand[1]);
                require(std::fabs(back[0] - x) < 0.5f && std::fabs(back[1] - y) < 0.5f, "hand_for_cursor inverts the model");
                require(hand[0] >= -0.4f && hand[0] <= 0.7f && hand[1] > -0.1f, "every pointed hand is a raised one");
            }
        sfr::NuiSkeletonEmulation pointing;
        pointing.point_right(1000.0f, 600.0f);
        pointing.update({});
        require(pointing.hand(true)[1] > 0.4f, "pointing raises a resting hand first");
        for (int i = 0; i < 30; ++i) pointing.update({});
        const auto target = sfr::NuiSkeletonEmulation::hand_for_cursor(1000.0f, 600.0f);
        require(std::fabs(pointing.hand(true)[0] - target[0]) < 1e-6f && std::fabs(pointing.hand(true)[1] - target[1]) < 1e-6f,
                "the raise ends where the pointer is");
        pointing.update({});
        require(std::fabs(pointing.hand(true)[0] - target[0]) < 1e-6f, "the pointed hand stays without input");
        pointing.point_right(100.0f, 700.0f);
        const auto corner = sfr::NuiSkeletonEmulation::hand_for_cursor(100.0f, 700.0f);
        require(pointing.hand(true)[0] == corner[0] && pointing.hand(true)[2] == 2.25f, "a raised hand goes at once");
        pointing.update({});
        require(pointing.hand(true)[0] == corner[0], "the lower left corner is within the hand's reach");
        pad = {};
        pad.thumb_rx = 32767;
        pointing.update(pad);
        require(pointing.hand(true)[0] > corner[0], "the stick moves on from a pointed hand");
        require(!pointing.pointed(), "a hand the stick moved is no longer the pointer's");
        pointing.point_right(1125.0f, 68.0f);
        require(pointing.pointed(), "a pointed hand is the pointer's");
        pointing.park_pointed();
        const auto waiting = pointing.hand(true);
        require(!pointing.pointed() && waiting[0] < 0.0f && waiting[1] > 0.5f, "a chosen button's cursor goes to its corner");
        pointing.park_pointed();
        require(pointing.hand(true) == waiting, "parking twice changes nothing");
        sfr::NuiSkeletonEmulation raising;
        raising.point_right(640.0f, 360.0f);
        raising.park_pointed();
        for (int i = 0; i < 31; ++i) raising.update({});
        require(raising.hand(true) == waiting, "a raise for a pointer parked meanwhile ends in the corner");
        // A page with its cursor hidden wants a new raise from the arm down.
        raising.point_right(852.0f, 460.0f, true);
        raising.update({});
        require(raising.hand(true)[1] < 0.0f, "a raise again starts with the arm down");
        for (int i = 0; i < 6; ++i) raising.update({});
        require(raising.hand(true)[1] > 0.4f, "then the arm goes up");
        for (int i = 0; i < 30; ++i) raising.update({});
        const auto item = sfr::NuiSkeletonEmulation::hand_for_cursor(852.0f, 460.0f);
        require(raising.hand(true)[0] == item[0] && raising.hand(true)[1] == item[1], "and then to the item");

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
