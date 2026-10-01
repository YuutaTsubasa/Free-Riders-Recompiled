#include "diagnostic_hooks.h"
#include "ppc_recomp_shared.h"
#include "race_frame_clock.h"
#include <bit>
#include <chrono>
#include <cstdlib>
#include <iostream>

namespace {
bool realtime_races() {
    static const bool enabled = [] {
        if (const char* value = std::getenv("SFR_REALTIME_RACE")) return *value && *value != '0';
#ifdef __ANDROID__
        return true;
#else
        return false;
#endif
    }();
    return enabled;
}
}

PPC_FUNC_IMPL(__imp__sub_824B1588);
// The original clock already exposes seconds, milliseconds and normalized
// 60 Hz frames to simulation and animation. Its forced step of 1 overrides
// elapsed time, slowing the whole race whenever rendering misses 60 FPS.
SFR_CONCURRENT_HOOK(sub_824B1588) {
    const uint32_t clock = ctx.r3.u32;
    if (!realtime_races() || sfr::current_guest_thread_id() != 1) {
        __imp__sub_824B1588(ctx, base);
        return;
    }
    const uint64_t now_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    __imp__sub_824B1588(ctx, base);

    auto& memory = *sfr::active_memory;
    static thread_local sfr::RaceFrameClock elapsed;
    uint64_t scene = 0;
    // Restrict the override to the title's known 60 Hz race clock. Preserve
    // any explicit alternative step/rate/scale, including future game modes.
    if (clock == 0x83E53810 && memory.load<uint32_t>(clock) == 0x821AAA50 &&
        memory.load<uint32_t>(clock+4) == 0x3F800000 &&
        memory.load<uint32_t>(clock+12) == 0x42700000 &&
        memory.load<uint32_t>(clock+16) == 0x3C888889 &&
        memory.load<uint32_t>(clock+20) == 0x3F800000) {
        if (const uint32_t race = memory.load<uint32_t>(0x83E52F8C))
            scene = (uint64_t(clock) << 32) | race;
    }
    const auto step = elapsed.update(now_ns, scene);
    if (!step) return;

    const float seconds = step->frames * (1.0f / 60.0f);
    memory.store<uint32_t>(clock+32, std::bit_cast<uint32_t>(seconds * 1000.0f));
    memory.store<uint32_t>(clock+36, std::bit_cast<uint32_t>(seconds));
    memory.store<uint32_t>(clock+40, std::bit_cast<uint32_t>(step->frames));
    memory.store<uint8_t>(clock+91, step->frames > 1.0f ? 1 : 0);
    // Keep +4 untouched: legacy controller gestures also read that setting.
    // +44/+48/+52/+92 retain the original clock's computed diagnostic values.
    if (step->clamped && step->elapsed_seconds > 0.25)
        std::cerr << "RACE_CLOCK clamped_seconds=" << step->elapsed_seconds
                  << " applied_seconds=" << seconds << '\n';
}
