#include "sony_gamepad.h"
#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#endif

namespace sfr::sony {
bool supported(uint16_t product) {
    return product == dualsense || product == dualsense_edge || product == dualshock4 || product == dualshock4_v2;
}

namespace {
// Offsets in a report: the four stick bytes, the triggers, and the two
// button bytes (hat and face buttons, then shoulders and the rest).
struct Layout { size_t sticks, left_trigger, right_trigger, buttons; };

int16_t stick(uint8_t value) {
    // 0..255 with 128 at rest, to the XInput range.
    const int centred = int(value) - 128;
    return int16_t(centred >= 0 ? centred * 32767 / 127 : centred * 256);
}
// The vertical axes read 0 at the top; XInput's up is positive.
int16_t upward(uint8_t value) {
    return int16_t((std::min)(-int(stick(value)), 32767));
}
}

std::optional<GamepadState> parse_report(uint16_t product, uint32_t report_length, std::span<const uint8_t> report) {
    if (report.empty()) return std::nullopt;
    const bool five = product == dualsense || product == dualsense_edge;
    Layout layout{};
    switch (report[0]) {
    case 0x01:
        // A DualSense on USB sends its full report as 0x01; on Bluetooth, and
        // a DualShock 4 either way, 0x01 is the short report.
        layout = five && report_length == 64 ? Layout{1, 5, 6, 8} : Layout{1, 8, 9, 5};
        break;
    case 0x31: layout = Layout{2, 6, 7, 9}; break;    // DualSense, Bluetooth full report
    case 0x11: layout = Layout{3, 10, 11, 7}; break;  // DualShock 4, Bluetooth full report
    default: return std::nullopt;
    }
    if (report.size() < (std::max)({layout.sticks + 4, layout.right_trigger + 1, layout.buttons + 2})) return std::nullopt;
    namespace button = gamepad_button;
    GamepadState state;
    state.thumb_lx = stick(report[layout.sticks]);
    state.thumb_ly = upward(report[layout.sticks + 1]);
    state.thumb_rx = stick(report[layout.sticks + 2]);
    state.thumb_ry = upward(report[layout.sticks + 3]);
    state.left_trigger = report[layout.left_trigger];
    state.right_trigger = report[layout.right_trigger];
    const uint8_t face = report[layout.buttons], rest = report[layout.buttons + 1];
    static constexpr uint16_t hat[8] = {
        button::dpad_up, button::dpad_up | button::dpad_right, button::dpad_right,
        button::dpad_right | button::dpad_down, button::dpad_down, button::dpad_down | button::dpad_left,
        button::dpad_left, button::dpad_left | button::dpad_up};
    if ((face & 0x0F) < 8) state.buttons |= hat[face & 0x0F];
    if (face & 0x10) state.buttons |= button::x;       // square
    if (face & 0x20) state.buttons |= button::a;       // cross
    if (face & 0x40) state.buttons |= button::b;       // circle
    if (face & 0x80) state.buttons |= button::y;       // triangle
    if (rest & 0x01) state.buttons |= button::left_shoulder;
    if (rest & 0x02) state.buttons |= button::right_shoulder;
    if (rest & 0x10) state.buttons |= button::back;    // Create / Share
    if (rest & 0x20) state.buttons |= button::start;   // Options
    if (rest & 0x40) state.buttons |= button::left_thumb;
    if (rest & 0x80) state.buttons |= button::right_thumb;
    return state;
}

#ifdef _WIN32
namespace {
struct Reader {
    std::mutex mutex;
    std::optional<GamepadState> state;
};
Reader& reader() {
    static Reader instance;
    return instance;
}

// Opens the first supported controller's gamepad collection; its product
// and input report length.
HANDLE open_controller(uint16_t& product, uint32_t& report_length) {
    GUID guid;
    HidD_GetHidGuid(&guid);
    const HDEVINFO devices = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    HANDLE found = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA interface_data{sizeof(interface_data)};
    for (DWORD index = 0; found == INVALID_HANDLE_VALUE &&
                          SetupDiEnumDeviceInterfaces(devices, nullptr, &guid, index, &interface_data); ++index) {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, nullptr, 0, &size, nullptr);
        if (!size) continue;
        std::vector<uint8_t> buffer(size);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, detail, size, nullptr, nullptr)) continue;
        HANDLE device = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (device == INVALID_HANDLE_VALUE)
            device = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, 0, nullptr);
        if (device == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attributes{sizeof(attributes)};
        PHIDP_PREPARSED_DATA preparsed = nullptr;
        HIDP_CAPS caps{};
        const bool ours = HidD_GetAttributes(device, &attributes) && attributes.VendorID == vendor &&
                          supported(attributes.ProductID) && HidD_GetPreparsedData(device, &preparsed);
        if (ours) {
            HidP_GetCaps(preparsed, &caps);
            HidD_FreePreparsedData(preparsed);
        }
        // The gamepad collection (usage page 1, usage 5).
        if (ours && caps.UsagePage == 1 && caps.Usage == 5 && caps.InputReportByteLength) {
            product = attributes.ProductID;
            report_length = caps.InputReportByteLength;
            found = device;
        } else {
            CloseHandle(device);
        }
    }
    SetupDiDestroyDeviceInfoList(devices);
    return found;
}

void read_forever() {
    for (;;) {
        uint16_t product = 0;
        uint32_t length = 0;
        const HANDLE device = open_controller(product, length);
        if (device == INVALID_HANDLE_VALUE) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        std::vector<uint8_t> report(length);
        DWORD read = 0;
        while (ReadFile(device, report.data(), DWORD(report.size()), &read, nullptr)) {
            if (const auto state = parse_report(product, length, std::span<const uint8_t>(report.data(), read))) {
                std::lock_guard lock(reader().mutex);
                reader().state = state;
            }
        }
        // Unplugged, or the read failed: forget it and look again.
        CloseHandle(device);
        std::lock_guard lock(reader().mutex);
        reader().state.reset();
    }
}
}

std::optional<GamepadState> latest() {
    static const bool started = [] {
        std::thread(read_forever).detach();
        return true;
    }();
    (void)started;
    std::lock_guard lock(reader().mutex);
    return reader().state;
}
#else
std::optional<GamepadState> latest() { return std::nullopt; }
#endif
}
