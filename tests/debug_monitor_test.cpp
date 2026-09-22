#include "debug_monitor.h"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F action) {
    try { action(); } catch (const sfr::RuntimeStop&) { return; }
    throw std::runtime_error("unsupported debug monitor access was accepted");
}
}
int main() {
    try {
        sfr::GuestMemory memory;
        sfr::AbsentDebugMonitor monitor(memory);
        memory.map(0x10000, 4);
        memory.store<uint32_t>(0x10000, sfr::AbsentDebugMonitor::address);
        const auto cell = memory.load<uint32_t>(0x10000);
        require(cell != 0 && memory.load<uint32_t>(cell) == 0,
                "import points to a valid cell containing null, matching the absent monitor ABI");
        for (uint32_t i = 0; i < 4; ++i)
            require(memory.load<uint8_t>(cell + i) == 0, "absent monitor word is big-endian zero");
        rejects([&] { memory.store<uint32_t>(cell, 1); });
        rejects([&] { memory.load<uint8_t>(cell + 4); });
        rejects([&] { memory.load<uint32_t>(cell + 1); });
        rejects([&] { memory.load<uint32_t>(0); });
        rejects([&] { sfr::AbsentDebugMonitor duplicate(memory); });
        require(memory.load<uint32_t>(cell) == 0, "rejected writes and duplicate binding preserve absence");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
