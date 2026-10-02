#include "ppc_recomp_shared.h"
#include "diagnostic_hooks.h"
#include <bit>

PPC_FUNC_IMPL(__imp__sub_8236DD60);

// A channel transition can reach 82372AC4 before the saved heading at
// object+720 has been filled. With multi-frame race updates, Frozen Forest
// passes (0,0,0,1) here. The original transforms that direction, divides by
// its length in 8225EE88 and feeds NaN to both rider rotation interpolators.
// Retain the existing orientation until there is a direction to align to.
// Do not normalize/replace valid directions or alter the shared acos routine.
SFR_CONCURRENT_HOOK(sub_8236DD60) {
    auto& memory = *sfr::active_memory;
    const uint32_t direction = ctx.r4.u32;
    const auto component = [&](uint32_t offset) {
        return std::bit_cast<float>(memory.load<uint32_t>(uint64_t(direction) + offset));
    };
    if (component(0) == 0.0f && component(4) == 0.0f && component(8) == 0.0f)
        return;
    __imp__sub_8236DD60(ctx, base);
}
