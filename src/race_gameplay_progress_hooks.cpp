#include "race_gameplay_progress.h"
#include "diagnostic_hooks.h"
#include "ppc_recomp_shared.h"
#include <bit>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {
std::mutex progress_mutex;
sfr::RaceGameplayProgress progress;

// Read from the update's owner, never by invoking a guest getter/refcount.
sfr::GameplayObservation read_observation(uint32_t controller) {
    sfr::GameplayObservation value;
    auto& memory = *sfr::active_memory;
    const char* fixed_step = std::getenv("SFR_REALTIME_RACE");
    if (!fixed_step || std::strcmp(fixed_step, "0") != 0) return value;
    if (!memory.readable(0x83E52F8C, 4)) return value;
    value.object = memory.load<uint32_t>(0x83E52F8C);
    if (!value.object || (value.object & 3) || uint64_t(value.object) + 160 > 0x100000000ull ||
        !memory.readable(value.object, 160) ||
        memory.load<uint32_t>(uint64_t(value.object) + 80) != controller) return value;
    value.controller = controller;
    if (!controller || (controller & 7) || uint64_t(controller) + 2092 > 0x100000000ull ||
        !memory.readable(controller, 340) || !memory.readable(uint64_t(controller) + 2088, 4) ||
        memory.load<uint32_t>(controller) != 0x8219EB78 ||
        memory.load<uint32_t>(uint64_t(controller) + 12) != 4 ||
        !memory.readable(0x82B0569F, 1) || memory.load<uint8_t>(0x82B0569F) != 1 ||
        !memory.readable(0x83E53810, 44) ||
        memory.load<uint32_t>(0x83E53810) != 0x821AAA50 ||
        memory.load<uint32_t>(0x83E53814) != 0x3F800000) return value;
    value.phase = memory.load<uint32_t>(uint64_t(controller) + 8);
    constexpr std::array<uint32_t, 7> offsets{128, 132, 136, 140, 148, 152, 156};
    for (unsigned i = 0; i < offsets.size(); ++i)
        value.config[i] = memory.load<uint32_t>(uint64_t(value.object) + offsets[i]);
    // Config156==4 selects raw host clock timers; unknown variants are not
    // accepted by this fixed-step benchmark contract. This observed path is0.
    if (value.config[6] != 0 || memory.load<uint32_t>(uint64_t(controller) + 140) != 0) return value;
    // Preparation constructs the countdown as a generic timer before E50
    // sets direction/limit. Enforce those only at countdown/racing admission.
    if (value.phase >= 10 && (memory.load<uint32_t>(0x83E53838) != 0x3F800000 ||
        memory.load<uint32_t>(uint64_t(controller) + 332) != 1 ||
        std::bit_cast<double>(memory.load<uint64_t>(uint64_t(controller) + 304)) != 0)) return value;
    const uint32_t pause_owner = memory.load<uint32_t>(uint64_t(controller) + 2088);
    if (pause_owner) {
        if (uint64_t(pause_owner) + 124 > 0x100000000ull || !memory.readable(pause_owner, 124)) return value;
        value.paused = memory.load<uint8_t>(uint64_t(pause_owner) + 123) != 0;
    }
    value.elapsed_ms = std::bit_cast<double>(memory.load<uint64_t>(uint64_t(controller) + 88));
    value.countdown_ms = std::bit_cast<double>(memory.load<uint64_t>(uint64_t(controller) + 280));
    value.supported = true;
    return value;
}
}

namespace sfr {
bool gameplay_progress_enabled() {
    static const bool enabled = [] {
        const char* flag = std::getenv("SFR_GAMEPLAY_METRICS");
        const char* limit = std::getenv("SFR_GAMEPLAY_TICK_LIMIT");
        return (flag && *flag == '1') || (limit && std::strtoul(limit, nullptr, 10) > 0);
    }();
    return enabled;
}
GameplayProgress gameplay_progress_snapshot(uint32_t live_object) {
    std::lock_guard lock(progress_mutex);
    return progress.snapshot(live_object);
}
}

PPC_FUNC_IMPL(__imp__sub_82246818);
SFR_CONCURRENT_HOOK(sub_82246818) {
    const uint32_t object = ctx.r3.u32;
    __imp__sub_82246818(ctx, base);
    if (sfr::gameplay_progress_enabled()) {
        std::lock_guard lock(progress_mutex);
        progress.constructed(object);
    }
}

PPC_FUNC_IMPL(__imp__sub_8224C8B0);
SFR_CONCURRENT_HOOK(sub_8224C8B0) {
    const uint32_t controller = ctx.r3.u32;
    __imp__sub_8224C8B0(ctx, base);
    if (sfr::gameplay_progress_enabled() && sfr::current_guest_thread_id() == 1 && sfr::active_memory) {
        const auto observation = read_observation(controller);
        std::lock_guard lock(progress_mutex);
        progress.observe(observation);
    }
}
