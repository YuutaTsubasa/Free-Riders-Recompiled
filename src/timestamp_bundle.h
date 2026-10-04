#pragma once
#include "guest_memory.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

namespace sfr {
class TimestampBundle {
public:
    static constexpr uint32_t address = 0x71400000;

    // The provider and its captures must remain valid for the memory's lifetime.
    TimestampBundle(GuestMemory& memory, std::function<uint32_t()> uptime);
    ~TimestampBundle();
    TimestampBundle(const TimestampBundle&) = delete;
    TimestampBundle& operator=(const TimestampBundle&) = delete;
private:
    // GuestMemory::direct_guest_access: generated code reads the uptime word
    // itself, so a host thread writes it every millisecond, as the console's
    // kernel does from its clock interrupt.
    std::atomic<bool> stop_{false};
    std::thread ticker_;
};
}
