#include "timestamp_bundle.h"
#include <chrono>
#include <string>
#include <utility>

namespace sfr {
TimestampBundle::TimestampBundle(GuestMemory& memory, std::function<uint32_t()> uptime) {
    if (!uptime) throw RuntimeStop("timestamp-bundle", address, "empty uptime provider");
    // Only +16 is confirmed by the observed title helper. Unknown prefix words
    // stay guarded and the remaining suffix is outside the logical mapping.
    memory.map(address, 20);
    for (uint32_t offset = 0; offset < 16; offset += 4)
        memory.add_import_variable(address + offset, "KeTimeStampBundle unknown field +" + std::to_string(offset));
    if (GuestMemory::direct_guest_access)
        ticker_ = std::thread([this, &memory, uptime] {
            while (!stop_.load(std::memory_order_relaxed)) {
                memory.refresh_word(address + 16, uptime());
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    memory.add_read_only_word(address + 16, std::move(uptime));
}

TimestampBundle::~TimestampBundle() {
    stop_.store(true, std::memory_order_relaxed);
    if (ticker_.joinable()) ticker_.join();
}
}
