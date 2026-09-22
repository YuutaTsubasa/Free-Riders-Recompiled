#include "native_input.h"
#include "sony_gamepad.h"
#include <vector>
#include "guest_memory.h"
#include <iostream>
#include <set>
#include <stdexcept>

namespace {
namespace button = sfr::gamepad_button;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

// PlayStation HID reports in the XInput layout.
void playstation_reports() {
    namespace button = sfr::gamepad_button;
    using sfr::sony::parse_report;
    // DualSense on USB: report 0x01, 64 bytes. Sticks at rest, cross and the
    // hat pointing up-right, L1 and Options, L2 half pulled.
    std::vector<uint8_t> usb(64, 0);
    usb[0] = 0x01; usb[1] = usb[2] = usb[3] = usb[4] = 128;
    usb[5] = 128; usb[8] = 0x20 | 1; usb[9] = 0x01 | 0x20;
    auto state = parse_report(sfr::sony::dualsense, 64, usb);
    require(state && state->thumb_lx == 0 && state->thumb_ly == 0 && state->thumb_rx == 0 && state->thumb_ry == 0,
            "sticks at rest are centred");
    require(state->buttons == (button::a | button::dpad_up | button::dpad_right | button::left_shoulder | button::start),
            "cross, hat, L1 and Options");
    require(state->left_trigger == 128 && state->right_trigger == 0, "triggers");
    // Stick extremes: full left and full up read as XInput's left and up.
    usb[1] = 0; usb[2] = 0; usb[8] = 8;  // hat released
    state = parse_report(sfr::sony::dualsense, 64, usb);
    require(state->thumb_lx == -32768 && state->thumb_ly == 32767 && !(state->buttons & 0x000F),
            "full left and up; a released hat is no D-pad");
    // DualSense on Bluetooth, full report 0x31: the same fields one byte later.
    std::vector<uint8_t> bt(78, 0);
    bt[0] = 0x31; bt[2] = 255; bt[3] = bt[4] = bt[5] = 128; bt[7] = 200; bt[9] = 0x40 | 8; bt[10] = 0x10;
    state = parse_report(sfr::sony::dualsense, 78, bt);
    require(state && state->thumb_lx == 32767 && state->right_trigger == 200 &&
            state->buttons == (button::b | button::back), "Bluetooth report: circle, Create, R2");
    // A DualSense on Bluetooth before its full report, or a DualShock 4: the
    // short 0x01 layout (triangle, square, R1).
    std::vector<uint8_t> short_report(64, 0);
    short_report[0] = 0x01; short_report[1] = short_report[2] = short_report[3] = short_report[4] = 128;
    short_report[5] = 0x80 | 0x10 | 8; short_report[6] = 0x02; short_report[9] = 77;
    for (const auto product : {sfr::sony::dualshock4, sfr::sony::dualsense}) {
        state = parse_report(product, product == sfr::sony::dualsense ? 78 : 64, short_report);
        require(state && state->buttons == (button::y | button::x | button::right_shoulder) && state->right_trigger == 77,
                "short report");
    }
    require(!parse_report(sfr::sony::dualsense, 64, std::vector<uint8_t>{0x05, 1, 2}), "other reports carry no controls");
}
template<class F> void stops(F operation, const char* message) {
    try { operation(); } catch (const sfr::RuntimeStop&) { return; }
    throw std::runtime_error(message);
}

sfr::GamepadState keys(std::set<int> held) {
    return sfr::keyboard_gamepad([&](int key) { return held.count(key) > 0; });
}

void run() {
    // Keyboard mapping (Windows virtual keys: arrows 0x25..0x28, Enter 0x0D).
    require(keys({}) == sfr::GamepadState{}, "no keys is rest");
    require(keys({0x0D}).buttons == button::start, "Enter is START");
    require(keys({'Z'}).buttons == button::a && keys({0x20}).buttons == button::a, "Z and Space are A");
    require(keys({'X'}).buttons == button::b && keys({0x1B}).buttons == button::b, "X and Escape are B");
    const auto up = keys({0x26});
    require(up.buttons == button::dpad_up && up.thumb_ly == 32767 && up.thumb_lx == 0, "Up is D-pad and stick");
    const auto left = keys({0x25});
    require(left.buttons == button::dpad_left && left.thumb_lx == -32768, "Left is D-pad and stick");
    require(keys({0x25, 0x27}).thumb_lx == 0, "opposite arrows cancel on the stick");
    require(keys({'F'}).right_trigger == 255 && keys({'R'}).left_trigger == 255 && keys({}).right_trigger == 0,
            "F and R are the triggers");
    require(keys({'I'}).thumb_ry == 32767 && keys({'J'}).thumb_rx == -32768 && keys({'I'}).buttons == 0,
            "I/J/K/L are the right stick");
    require(keys({'J', 'L'}).thumb_rx == 0, "opposite right-stick keys cancel");

    sfr::GamepadState pad{button::a, 10, 200, 1000, -30000, 0, 0};
    const auto merged = sfr::merge_gamepads(pad, keys({0x0D, 0x26}));
    require(merged.buttons == (button::a | button::start | button::dpad_up), "buttons are merged");
    require(merged.thumb_ly == 32767 && merged.thumb_lx == 1000, "axis farther from rest wins");
    require(merged.left_trigger == 10 && merged.right_trigger == 200, "triggers keep maximum");

    // Big-endian XINPUT_STATE layout.
    sfr::GuestMemory memory;
    memory.map(0x10000000, 0x100);
    sfr::write_xinput_state(memory, 0x10000010, 7, sfr::GamepadState{0x9011, 1, 2, -2, 3, -4, 5});
    require(memory.load<uint32_t>(0x10000010) == 7, "packet number");
    require(memory.load<uint16_t>(0x10000014) == 0x9011, "buttons");
    require(memory.load<uint8_t>(0x10000016) == 1 && memory.load<uint8_t>(0x10000017) == 2, "triggers");
    require(memory.load<uint16_t>(0x10000018) == 0xFFFE && memory.load<uint16_t>(0x1000001A) == 3 &&
            memory.load<uint16_t>(0x1000001C) == 0xFFFC && memory.load<uint16_t>(0x1000001E) == 5, "thumbs");
    stops([&] { sfr::write_xinput_state(memory, 0x100000F8, 0, {}); }, "partial output must stop");

    // Users: 0 is backed by the keyboard; others need a host controller.
    std::set<int> held;
    std::optional<sfr::GamepadState> pad1;
    sfr::NativeInput input([&](uint32_t user) { return user == 1 ? pad1 : std::nullopt; },
                           [&] { return keys(held); });
    require(input.get_state(memory, 0, 0x10000020) == sfr::xinput_success, "user 0 is connected");
    require(memory.load<uint32_t>(0x10000020) == 0 && memory.load<uint16_t>(0x10000024) == 0, "rest state");
    require(input.get_state(memory, 1, 0x10000030) == sfr::xinput_not_connected, "user 1 without pad");
    require(input.get_state(memory, 2, 0x10000030) == sfr::xinput_not_connected, "user 2 without pad");
    held = {0x0D};
    input.get_state(memory, 0, 0x10000020);
    require(memory.load<uint32_t>(0x10000020) == 1 && memory.load<uint16_t>(0x10000024) == button::start,
            "changed state advances the packet");
    input.get_state(memory, 0, 0x10000020);
    require(memory.load<uint32_t>(0x10000020) == 1, "unchanged state keeps the packet");
    held = {};
    input.get_state(memory, 0, 0x10000020);
    require(memory.load<uint32_t>(0x10000020) == 2 && memory.load<uint16_t>(0x10000024) == 0, "release is a change");
    pad1 = sfr::GamepadState{button::b};
    require(input.get_state(memory, 1, 0x10000030) == sfr::xinput_success &&
            memory.load<uint16_t>(0x10000034) == button::b && memory.load<uint32_t>(0x10000030) == 1,
            "host pad for user 1 with its own packet count");
    stops([&] { input.get_state(memory, 4, 0x10000030); }, "user 4 must stop");
    playstation_reports();
}
}

int main() {
    try {
        run();
        std::cout << "native input tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
