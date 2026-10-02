#include "ppc_recomp_shared.h"
#include "diagnostic_hooks.h"
#include "race_boarding.h"
#include <bit>

PPC_FUNC_IMPL(__imp__sub_82370588);

SFR_CONCURRENT_HOOK(sub_82370588) {
    auto& memory = *sfr::active_memory;
    const uint32_t object = ctx.r3.u32;
    const uint8_t entry_phase = memory.load<uint8_t>(uint64_t(object) + 500);
    const uint32_t services = memory.load<uint32_t>(0x83E516A0);
    const uint32_t clock = memory.load<uint32_t>(uint64_t(services) + 24);
    const float frames = std::bit_cast<float>(memory.load<uint32_t>(uint64_t(clock) + 40));
    __imp__sub_82370588(ctx, base);
    if (entry_phase != 1 || frames == 1.0f) return;

    // Match the rider lookup in the original update. Slot 356 equals 352 for
    // a single rider; the original uses the same position for both in that case.
    const uint32_t riders = memory.load<uint32_t>(0x83E52FDC);
    const uint32_t count = memory.load<uint32_t>(uint64_t(riders) + 20);
    const uint32_t begin = memory.load<uint32_t>(uint64_t(riders) + 36);
    const uint32_t end = memory.load<uint32_t>(uint64_t(riders) + 40);
    const uint32_t first_slot = memory.load<uint32_t>(uint64_t(object) + 352);
    const uint32_t second_slot = memory.load<uint32_t>(uint64_t(object) + 356);
    if (begin == end || first_slot >= count || second_slot >= count) return;
    const uint32_t first = memory.load<uint32_t>(uint64_t(begin) + first_slot * 4ull);
    const uint32_t second = memory.load<uint32_t>(uint64_t(begin) + second_slot * 4ull);
    sfr::correct_boarding_step(memory, object, first, second, frames, entry_phase);
}
