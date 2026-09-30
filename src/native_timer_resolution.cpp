#include "native_timer_resolution.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <timeapi.h>
#endif

namespace sfr {
namespace {
bool begin_period(void*, uint32_t period) noexcept {
#ifdef _WIN32
    return timeBeginPeriod(period) == TIMERR_NOERROR;
#else
    (void)period;
    return false;
#endif
}

void end_period(void*, uint32_t period) noexcept {
#ifdef _WIN32
    timeEndPeriod(period);
#else
    (void)period;
#endif
}
}

NativeTimerResolution::NativeTimerResolution(bool enabled) noexcept
    : NativeTimerResolution(enabled, {nullptr, begin_period, end_period}) {}

NativeTimerResolution::NativeTimerResolution(bool enabled, Api api) noexcept : api_(api) {
    if (enabled && api_.begin && api_.end) active_ = api_.begin(api_.context, period_ms);
}

NativeTimerResolution::~NativeTimerResolution() noexcept {
    if (active_) api_.end(api_.context, period_ms);
}
}
