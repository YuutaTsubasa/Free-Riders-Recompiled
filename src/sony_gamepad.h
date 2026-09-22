#pragma once
#include "native_input.h"
#include <cstdint>
#include <optional>
#include <span>

namespace sfr {
// PlayStation controllers, which Windows does not offer through XInput:
// DualSense (PS5) and DualShock 4 (PS4), over USB or Bluetooth.
namespace sony {
constexpr uint16_t vendor = 0x054C;
constexpr uint16_t dualsense = 0x0CE6, dualsense_edge = 0x0DF2;
constexpr uint16_t dualshock4 = 0x05C4, dualshock4_v2 = 0x09CC;
bool supported(uint16_t product);
// One HID input report (report ID first, as ReadFile returns it) in the
// XInput layout: cross A, circle B, square X, triangle Y, L1/R1 shoulders,
// L2/R2 triggers, Options START, Create/Share BACK, L3/R3 thumbs, the hat
// as the D-pad. report_length is the device's input report length, which
// tells a DualSense on USB (64) from one on Bluetooth. Null for reports
// that carry no controls.
std::optional<GamepadState> parse_report(uint16_t product, uint32_t report_length, std::span<const uint8_t> report);
// The newest state of the first connected PlayStation controller, read by
// a background thread that finds one and reconnects when it is unplugged.
// Null when none is connected.
std::optional<GamepadState> latest();
}
}
