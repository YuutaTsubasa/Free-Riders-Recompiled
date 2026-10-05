#include "diagnostic_hooks.h"
#include "ppc_recomp_shared.h"
#include "race_frame_clock.h"
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <iostream>

namespace {
// The race clock runs on guest 1, while UI timelines run on guest 16.
// Publish one completed clock sample for both threads.
std::atomic<float> race_ui_frames{1.0f};
thread_local bool updating_race_ui = false;

// Android steps the race by the time that passed (SFR_REALTIME_RACE=0 turns
// it off), so a frame rate under 60 does not slow the race. Desktops, held
// at the launcher's 60 fps, step one original frame at a time unless
// SFR_REALTIME_RACE=1: v0.5.0 made elapsed time the default there too, and
// a charged jump off the side of Rocky Ridge then flew out of the course.
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

bool realtime_ui() {
    static const bool enabled = [] {
        const char* value = std::getenv("SFR_REALTIME_UI");
        return !value || (*value && *value != '0');
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
    if (!step) {
        race_ui_frames.store(1.0f, std::memory_order_relaxed);
        return;
    }

    const float seconds = step->frames * (1.0f / 60.0f);
    memory.store<uint32_t>(clock+32, std::bit_cast<uint32_t>(seconds * 1000.0f));
    memory.store<uint32_t>(clock+36, std::bit_cast<uint32_t>(seconds));
    memory.store<uint32_t>(clock+40, std::bit_cast<uint32_t>(step->frames));
    memory.store<uint8_t>(clock+91, step->frames > 1.0f ? 1 : 0);
    race_ui_frames.store(step->frames, std::memory_order_relaxed);
    // Keep +4 untouched: legacy controller gestures also read that setting.
    // +44/+48/+52/+92 retain the original clock's computed diagnostic values.
    if (step->clamped && step->elapsed_seconds > 0.25)
        std::cerr << "RACE_CLOCK clamped_seconds=" << step->elapsed_seconds
                  << " applied_seconds=" << seconds << '\n';
}

PPC_FUNC_IMPL(__imp__sub_824AE2A0);
// UI timelines (including the jump rating) use their own fixed playback
// speed, not clock+40. Each original call evaluates this frame's actions,
// then advances the timeline by at most one frame. Replaying bounded steps
// preserves intermediate events; multiplying the position alone skips them.
SFR_CONCURRENT_HOOK(sub_824AE2A0) {
    const float frames = race_ui_frames.load(std::memory_order_relaxed);
    if (!realtime_ui() || updating_race_ui || frames == 1.0f) {
        __imp__sub_824AE2A0(ctx, base);
        return;
    }
    auto& memory = *sfr::active_memory;
    const uint32_t wrapper = ctx.r3.u32, parameters = ctx.r4.u32;
    if (!memory.load<uint32_t>(wrapper+4)) {
        __imp__sub_824AE2A0(ctx, base);
        return;
    }
    const uint32_t model = memory.load<uint32_t>(wrapper+36);
    const uint32_t vtable = model ? memory.load<uint32_t>(model) : 0;
    const uint32_t rate_bits = memory.load<uint32_t>(wrapper+40);
    const float rate = std::bit_cast<float>(rate_bits);
    // These two known timeline classes evaluate through 824ACD78 and
    // advance through 824AE728. Preserve other implementations and explicit
    // reverse/faster-than-one-frame playback modes.
    if ((vtable != 0x821A9DD4 && vtable != 0x821AA0D4) ||
        !(rate > 0.0f && rate <= 1.0f)) {
        __imp__sub_824AE2A0(ctx, base);
        return;
    }
    updating_race_ui = true;
    try {
        sfr::for_each_race_ui_step(frames, [&](float step) {
            memory.store<uint32_t>(wrapper+40, std::bit_cast<uint32_t>(rate*step));
            ctx.r3.u64 = wrapper;
            ctx.r4.u64 = parameters;
            __imp__sub_824AE2A0(ctx, base);
            // Deliver completion to the owner before another animation cycle.
            return ctx.r3.u32 == 0;
        });
    } catch (...) {
        updating_race_ui = false;
        memory.store<uint32_t>(wrapper+40, rate_bits);
        throw;
    }
    updating_race_ui = false;
    memory.store<uint32_t>(wrapper+40, rate_bits);
}
