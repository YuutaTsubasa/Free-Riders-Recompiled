#pragma once
#include <array>
#include <cstdint>
#include <initializer_list>
#include <ostream>
#include <span>
#include <string_view>
#include <utility>

namespace sfr {
// Runtime integration; implemented beside the guest execution permit.
void flush_main_wait_trace(uint32_t frame, bool force = false);
void traced_host_wait(void (*wait)(void*), void* argument, const char* kind, int64_t timeout_ns);
struct WaitTargets {
    std::array<uint32_t, 64> values{};
    uint32_t count = 0;
    WaitTargets() = default;
    WaitTargets(std::initializer_list<uint32_t> list) : WaitTargets(std::span(list.begin(), list.size())) {}
    explicit WaitTargets(std::span<const uint32_t> list) {
        for (auto value : list) { if (count == values.size()) break; values[count++] = value; }
    }
    bool operator==(const WaitTargets&) const = default;
};
struct WaitSite {
    std::string_view kind;
    uint32_t caller = 0;
    WaitTargets targets;
    int64_t timeout_ns = -1; // -1: infinite/unknown; native timeout after rounding.
    uint32_t owner = 0; // Critical-section owner guest object, not a host thread ID.
    bool operator==(const WaitSite&) const = default;
};
struct WaitTraceRow {
    WaitSite site;
    uint32_t status = 0;
    uint64_t count = 0, failed = 0;
    uint64_t before_ns = 0, native_ns = 0, resume_ns = 0, overshoot_ns = 0, max_native_ns = 0;
    int64_t requested_min_ns = -1, requested_max_ns = -1;
};
// Single-thread-owned collector. No heap allocation or cross-thread locks.
class WaitTrace {
public:
    static constexpr size_t capacity = 128;
    void record(const WaitSite& site, uint32_t status, uint64_t before, uint64_t native,
                uint64_t resume, uint64_t total, bool failed) noexcept;
    void write_and_reset(std::ostream& out, uint32_t frame, double window_ms, uint32_t guest_id = 1);
    std::span<const WaitTraceRow> rows() const { return {rows_.data(), size_}; }
    size_t size() const { return size_; }
    uint64_t dropped() const { return dropped_; }
private:
    std::array<WaitTraceRow, capacity> rows_{};
    size_t size_ = 0;
    uint64_t dropped_ = 0;
    uint64_t dropped_total_ns_ = 0, dropped_native_ns_ = 0, dropped_resume_ns_ = 0;
};

// Runner retains its original ownership/cancellation rules. Clock injection
// permits deterministic tests; production uses steady_clock nanoseconds.
template<class Runner, class Operation, class Clock>
void observe_wait(bool enabled, WaitTrace& trace, const WaitSite& site,
                  Runner&& runner, Operation&& operation, Clock&& clock,
                  const uint32_t* status = nullptr) {
    if (!enabled) { runner(std::forward<Operation>(operation)); return; }
    const auto start = clock();
    uint64_t native_start = start, native_end = start;
    bool entered = false, returned = false;
    try {
        runner([&](auto&&... args) {
            entered = true; native_start = clock();
            try { operation(std::forward<decltype(args)>(args)...); }
            catch (...) { native_end = clock(); returned = true; throw; }
            native_end = clock(); returned = true;
        });
    } catch (...) {
        const auto end = clock();
        trace.record(site, status ? *status : 0, entered ? native_start-start : end-start,
                     returned ? native_end-native_start : 0, returned ? end-native_end : 0, end-start, true);
        throw;
    }
    const auto end = clock();
    trace.record(site, status ? *status : 0, native_start-start, native_end-native_start,
                 end-native_end, end-start, false);
}
}
