#include "ppc_recomp_shared.h"
#include "diagnostic_hooks.h"
#include "race_scripted_motion.h"

PPC_FUNC_IMPL(__imp__sub_822AB3B0);
SFR_CONCURRENT_HOOK(sub_822AB3B0) {
    auto& memory=*sfr::active_memory;
    const uint32_t physics=ctx.r3.u32;
    if(memory.load<uint32_t>(uint64_t(physics)+2392)!=1) {
        __imp__sub_822AB3B0(ctx,base);return;
    }
    const uint32_t services=memory.load<uint32_t>(0x83E516A0);
    const uint32_t clock=memory.load<uint32_t>(uint64_t(services)+24);
    const float frames=std::bit_cast<float>(memory.load<uint32_t>(uint64_t(clock)+40));
    sfr::with_scripted_force_step(memory,physics,frames,[&]{__imp__sub_822AB3B0(ctx,base);});
}

PPC_FUNC_IMPL(__imp__sub_8231CA00);
SFR_CONCURRENT_HOOK(sub_8231CA00) {
    sfr::with_scripted_path_update(*sfr::active_memory,ctx.r3.u32,[&]{__imp__sub_8231CA00(ctx,base);});
}
