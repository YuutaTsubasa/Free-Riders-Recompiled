#pragma once
#include "guest_memory.h"

namespace sfr {
// No Xbox remote-debug-monitor protocol or callback is installed in this runtime.
class AbsentDebugMonitor {
public:
    static constexpr uint32_t address = 0x72400000;
    explicit AbsentDebugMonitor(GuestMemory& memory);
};
}
