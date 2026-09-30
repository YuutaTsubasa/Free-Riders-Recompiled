#pragma once

#include <cstdint>

namespace sfr {

// Owns one successful timer-resolution request. Construct before starting the
// runtime's workers so the request outlives their short host waits.
class NativeTimerResolution {
public:
    static constexpr uint32_t period_ms = 1;

    struct Api {
        void* context;
        bool (*begin)(void*, uint32_t) noexcept;
        void (*end)(void*, uint32_t) noexcept;
    };

    explicit NativeTimerResolution(bool enabled = true) noexcept;
    NativeTimerResolution(bool enabled, Api api) noexcept;
    ~NativeTimerResolution() noexcept;

    NativeTimerResolution(const NativeTimerResolution&) = delete;
    NativeTimerResolution& operator=(const NativeTimerResolution&) = delete;
    NativeTimerResolution(NativeTimerResolution&&) = delete;
    NativeTimerResolution& operator=(NativeTimerResolution&&) = delete;

    [[nodiscard]] bool active() const noexcept { return active_; }

private:
    Api api_;
    bool active_ = false;
};

}
