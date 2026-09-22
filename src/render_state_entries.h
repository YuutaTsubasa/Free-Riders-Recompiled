#pragma once
#include <cstdint>

namespace sfr {
// Complete ABI-level setters: their state effects depend on device/arguments, not
// the original caller's LR. Keep partial and callback-based bridges separate.
constexpr bool is_native_render_state_entry(uint32_t address) {
    return address == 0x824E6A08 || address == 0x824E6EC8 || address == 0x824E6E68 ||
           address == 0x824E70D0 || address == 0x824E7140 || address == 0x824E7110 || address == 0x824E69A8 ||
           address == 0x824E6A40 || address == 0x824E6B60 || address == 0x824E6BF0 || address == 0x824E6AD0 ||
           address == 0x824E8248 || address == 0x824E83F0;
}
}
