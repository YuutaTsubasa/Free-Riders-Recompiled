#include "diagnostic_hooks.h"
#include "ppc_recomp_shared.h"
#include "race_frame_clock.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <unordered_map>
#include <mutex>

namespace {
// The race clock runs on guest 1, while UI timelines run on guest 16.
// Publish one completed clock sample for both threads.
std::atomic<float> race_ui_frames{1.0f};
thread_local bool updating_race_ui = false;
// Whole frames the running short step completed (RaceWholeFrames), -1 when
// a race is not stepping short: guest 1 writes it, its workers read it.
std::atomic<int32_t> race_whole_frames{-1};
// The unit the race clock (+20) was given for a short step, 0 when none.
std::atomic<uint32_t> race_unit_written{0};
// The running short step in frames, 0 when a race is not stepping short.
std::atomic<float> race_short_step{0.0f};
// Counts the race's steps (guest 1 writes it), for hooks that remember an
// object from one step to the next.
std::atomic<uint32_t> race_step_serial{0};

// Android steps the race by the time that passed (SFR_REALTIME_RACE=0 turns
// it off), so a frame rate under 60 does not slow the race. Desktops, held
// at the launcher's 60 fps, step one original frame at a time unless
// SFR_REALTIME_RACE=1: v0.5.0 made elapsed time the default there too, and
// a charged jump off the side of Rocky Ridge then flew out of the course.
bool realtime_races() {
    static const bool enabled = [] {
        if (sfr::race_step_rate() != 60) return true;
        if (const char* value = std::getenv("SFR_REALTIME_RACE")) return *value && *value != '0';
#ifdef __ANDROID__
        // Frame skipping (race_frame_clock.h) keeps the race's speed with the
        // console's fixed step instead.
        return !sfr::race_frameskip_enabled();
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
    // The race clock's unit (+20) as the original keeps it, 1, whenever the
    // original runs: a short step's unit (below) lasts only until the next
    // update, so the original, the check below and anything after a race see
    // the clock they always did.
    if (const uint32_t written = race_unit_written.exchange(0, std::memory_order_relaxed);
        written && clock == 0x83E53810 && sfr::active_memory->load<uint32_t>(clock+20) == written)
        sfr::active_memory->store<uint32_t>(clock+20, 0x3F800000);
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
        race_whole_frames.store(-1, std::memory_order_relaxed);
        race_short_step.store(0.0f, std::memory_order_relaxed);
        return;
    }

    const float frames = sfr::race_step_rate() != 60 ? 60.0f / float(sfr::race_step_rate()) : step->frames;
    const float seconds = frames * (1.0f / 60.0f);
    memory.store<uint32_t>(clock+32, std::bit_cast<uint32_t>(seconds * 1000.0f));
    memory.store<uint32_t>(clock+36, std::bit_cast<uint32_t>(seconds));
    memory.store<uint32_t>(clock+40, std::bit_cast<uint32_t>(frames));
    memory.store<uint8_t>(clock+91, frames > 1.0f ? 1 : 0);
    race_ui_frames.store(frames, std::memory_order_relaxed);
    static thread_local sfr::RaceWholeFrames whole;
    if (sfr::race_step_rate() != 60) {
        race_whole_frames.store(whole.update(frames), std::memory_order_relaxed);
        race_short_step.store(frames, std::memory_order_relaxed);
        race_step_serial.fetch_add(1, std::memory_order_relaxed);
        // +20 is the clock's unit: the original makes each step a whole number
        // of units (one at 60 Hz). A few readers take it as "one step" rather
        // than +40: the race camera's smoothing (8227BB98 and 8227B5D0 scale
        // a per-step blend by it and divide its durations in frames by it)
        // and timers that add it each step (822D3B20, 82379B30, 82379D40).
        // Left at 1, a short step ran all of them 60/N times too fast.
        memory.store<uint32_t>(clock+20, std::bit_cast<uint32_t>(frames));
        race_unit_written.store(std::bit_cast<uint32_t>(frames), std::memory_order_relaxed);
    }
    // Keep +4 untouched: legacy controller gestures also read that setting.
    // +44/+48/+52/+92 retain the original clock's computed diagnostic values.
    if (step->clamped && step->elapsed_seconds > 0.25)
        std::cerr << "RACE_CLOCK clamped_seconds=" << step->elapsed_seconds
                  << " applied_seconds=" << seconds << '\n';
}

// config/freeriders.toml: after an fctiwz or fctidz of the step, whose result
// is read back from the register's low word.
void RaceWholeFramesMidAsmHook(PPCRegister& truncated) {
    const int32_t whole = race_whole_frames.load(std::memory_order_relaxed);
    if (whole >= 0) truncated.u64 = uint64_t(whole);
}

// config/freeriders.toml: a distance the title moves something by every step,
// written for steps of one frame (it is not multiplied by the step). Scaled
// to the short step; at 60 Hz nothing changes.
void RaceShortStepScaleMidAsmHook(PPCRegister& amount) {
    const float step = race_short_step.load(std::memory_order_relaxed);
    if (step > 0) amount.f64 = double(float(amount.f64 * step));
}

// config/freeriders.toml: the step (clock +40) just loaded into a register
// for an amount the title computes once and then applies every update times
// the step again. At one frame a step the extra factor was 1; a short step
// applied it twice. The register gets one frame instead.
void RaceStepAsFrameMidAsmHook(PPCRegister& step) {
    if (race_short_step.load(std::memory_order_relaxed) > 0) step.f64 = 1.0;
}

// config/freeriders.toml, 8235AA48 at 8235AB48: a minecart's lever (Rocky
// Ridge) turns to the side the rider leans (+352, 0.41 rad either way) over a
// time in frames (+364: 20, or 10 back to the middle) at a speed per update
// (+360, the angle left over the time). The time counts down by the step, but
// each update added the whole speed: a short step turned it 60/N times as
// far, and each lean the other way overshot more than the last, until the
// lever hung upside down. Before the add, speed (f13) is scaled to the frames
// this update spent of the time left (left, f11: the time after this update's
// step, below zero on the last one).
void RaceLeverTurnMidAsmHook(PPCRegister& speed, PPCRegister& left) {
    const float step = race_short_step.load(std::memory_order_relaxed);
    if (step <= 0) return;
    const float frames = std::clamp(float(left.f64) + step, 0.0f, step);
    speed.f64 = double(float(speed.f64 * frames));
}

// config/freeriders.toml: moved = from +/- a fixed distance a step (a grab
// pole lining its racer up); only the distance is scaled.
void RaceShortStepMoveMidAsmHook(PPCRegister& moved, PPCRegister& from) {
    const float step = race_short_step.load(std::memory_order_relaxed);
    if (step > 0) moved.f64 = double(float(from.f64 + (moved.f64 - from.f64) * step));
}

// config/freeriders.toml: accumulated += amount and accumulated -= amount, an
// acceleration the title adds every step (Final Factory's skydive speeds its
// racer up by 0.0062 a frame, 8239F518). Before the instruction, so that it
// adds amount * step instead; the amount register itself is a constant the
// loop around it reuses for every racer, so it is not touched.
void RaceShortStepAddMidAsmHook(PPCRegister& accumulated, PPCRegister& amount) {
    const float step = race_short_step.load(std::memory_order_relaxed);
    if (step > 0) accumulated.f64 = double(float(accumulated.f64 + amount.f64 * (step - 1.0f)));
}

void RaceShortStepSubtractMidAsmHook(PPCRegister& accumulated, PPCRegister& amount) {
    const float step = race_short_step.load(std::memory_order_relaxed);
    if (step > 0) accumulated.f64 = double(float(accumulated.f64 - amount.f64 * (step - 1.0f)));
}

// config/freeriders.toml, 8231CA00 at 8231CC40: an object that runs a racer
// along a path (Metropolis Speedway Expert's launch up the building, the drop
// back down) copies the speed it slowed to (+232) into the speed it travels at
// (+236) on every update its path segment (+90, read before this update's
// travel) is in a range the course sets. Where the range starts, the launch
// slows to a crawl. At one frame a step the copy came after a whole frame at
// the old speed past that point; a short step travels less in its update, and
// at 144 Hz the racer started to crawl 7 units lower. From there it crawled 10
// units, not 3, to the top, and stayed above y 420 for 5.5 seconds, not 3.1.
// The copy waits until the racer has spent a frame in the range, as at 60 Hz:
// after the load of +232 (f0), which 8231CC44 stores to +236, the hook gives
// f0 the speed +236 already has until then.
void RaceLaunchSpeedHoldMidAsmHook(PPCRegister& speed, PPCRegister& r31) {
    const float step = race_short_step.load(std::memory_order_relaxed);
    if (step <= 0) return;
    const uint32_t serial = race_step_serial.load(std::memory_order_relaxed);
    struct Held { uint32_t serial; float frames; };
    static std::mutex mutex;
    static std::unordered_map<uint32_t, Held> held;
    std::lock_guard lock(mutex);
    if (held.size() > 64) held.clear();
    auto& entry = held[r31.u32];
    // A first update in the range, or one after leaving it, starts again.
    if (entry.serial + 1 != serial) entry.frames = 0;
    entry.serial = serial;
    entry.frames += step;
    if (entry.frames < 0.999f)
        speed.f64 = double(std::bit_cast<float>(sfr::active_memory->load<uint32_t>(r31.u32 + 236)));
}

// config/freeriders.toml, 822A9F58 at 822AA2D0: a racer's motion for this
// step, at r1+96. The original multiplies it by the step (822AA2B8) unless the
// racer's state (+2376) is 0, the state in which a course object carries it
// (the launcher after Forgotten Tomb's falling pillars, the start's run-in):
// at a fixed step of one frame that was the same thing. With its mode (+2392)
// 0 the motion is the racer's own speed along its heading, per frame; left
// unscaled, a short step moved it a whole frame's distance, 2.4 times as far
// a second at 144 Hz. In the other modes the motion is to a point the object
// sets (822EBA98 for grab poles), which the object's own hooks scale.
void RaceCarriedMotionMidAsmHook(PPCRegister& r1, PPCRegister& r30) {
    const float step = race_short_step.load(std::memory_order_relaxed);
    if (step <= 0) return;
    auto& memory = *sfr::active_memory;
    if (memory.load<uint32_t>(r30.u32 + 2376) != 0 || memory.load<uint32_t>(r30.u32 + 2392) != 0) return;
    for (const uint32_t offset : {96u, 100u, 104u}) {
        const uint32_t address = r1.u32 + offset;
        memory.store<uint32_t>(address, std::bit_cast<uint32_t>(std::bit_cast<float>(memory.load<uint32_t>(address)) * step));
    }
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
