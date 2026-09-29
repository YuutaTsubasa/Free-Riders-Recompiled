#pragma once
#include <array>
#include <atomic>
#include <cstdint>

namespace sfr {
// Decide whether a polled input state needs another diagnostic line.
class InputTrace {
public:
    explicit InputTrace(bool trace_packets = true) : trace_packets_(trace_packets) {
        // NativeInput returns success (0) or not-connected (0x48f), so this
        // cannot match an observed status, even when the packet wraps.
        for (auto& state : states_) state.store(UINT64_MAX, std::memory_order_relaxed);
    }

    bool changed(uint32_t user, uint32_t status, uint32_t packet) {
        // A disconnected query has no packet. Include its status so a
        // connection change is visible even when the last packet was zero.
        const uint64_t state = (uint64_t{status} << 32) | (status == 0 && trace_packets_ ? packet : 0);
        return states_.at(user).exchange(state, std::memory_order_relaxed) != state;
    }
private:
    bool trace_packets_;
    std::array<std::atomic<uint64_t>, 4> states_;
};
}
