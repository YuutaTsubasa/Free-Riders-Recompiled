#include "ppc_recomp_shared.h"
#include "diagnostic_hooks.h"
#include "race_launch.h"

PPC_FUNC_IMPL(__imp__sub_8238D108);
PPC_FUNC_IMPL(__imp__sub_8238DFB0);
PPC_FUNC_IMPL(__imp__sub_8238E620);
PPC_FUNC_IMPL(__imp__sub_8238C868);

SFR_CONCURRENT_HOOK(sub_8238D108) {
    sfr::with_launch_peer_countdowns(*sfr::active_memory,ctx.r3.u32,[&] {
        __imp__sub_8238D108(ctx,base);
    });
}

SFR_CONCURRENT_HOOK(sub_8238DFB0) {
    auto& memory=*sfr::active_memory;
    const uint32_t record=ctx.r3.u32;
    const uint32_t owner=memory.load<uint32_t>(uint64_t(record)+4);
    sfr::with_active_launch_phase(memory,owner,record,false,[&] {
        __imp__sub_8238DFB0(ctx,base);
    });
}

SFR_CONCURRENT_HOOK(sub_8238E620) {
    auto& memory=*sfr::active_memory;
    const uint32_t record=ctx.r3.u32;
    const uint32_t owner=memory.load<uint32_t>(uint64_t(record)+4);
    sfr::with_active_launch_phase(memory,owner,record,false,[&] {
        __imp__sub_8238E620(ctx,base);
    });
}

SFR_CONCURRENT_HOOK(sub_8238C868) {
    // A single peer's removal or landing must not restart the active launch.
    // Constructor and whole-cohort resets retain the original behavior.
    const uint32_t caller=uint32_t(ctx.lr);
    if(caller!=0x8238E9DC && caller!=0x8238D36C) {
        __imp__sub_8238C868(ctx,base);
        return;
    }
    sfr::with_active_launch_phase(*sfr::active_memory,ctx.r3.u32,0,true,[&] {
        __imp__sub_8238C868(ctx,base);
    });
}
