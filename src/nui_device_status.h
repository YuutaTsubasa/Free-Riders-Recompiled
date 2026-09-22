#pragma once
#include <cstdint>

namespace sfr {
class GuestMemory;
// This runtime has no connected Kinect backend. The original 24-byte result
// describes that absence; it is not a synthetic sensor or input frame.
void write_absent_nui_device_status(GuestMemory& memory, uint32_t output);
// The emulated sensor (nui_hooks.cpp): status (+12) bits 0 and 1 set, which
// the title tests as connected and ready before using the NUI library.
constexpr uint32_t emulated_nui_device_status = 3;
void write_nui_device_status(GuestMemory& memory, uint32_t output, uint32_t status);
}
