#pragma once
#include "guest_memory.h"
#include <cstdint>
#include <functional>

namespace sfr {
class TimestampBundle {
public:
    static constexpr uint32_t address = 0x71400000;

    // The provider and its captures must remain valid for the memory's lifetime.
    TimestampBundle(GuestMemory& memory, std::function<uint32_t()> uptime);
};
}
