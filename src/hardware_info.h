#pragma once

#include "guest_memory.h"

#include <cstdint>

namespace sfr {
class HardwareInfo {
public:
    static constexpr uint32_t address = 0x71200000;

    explicit HardwareInfo(GuestMemory& memory);
};
}
