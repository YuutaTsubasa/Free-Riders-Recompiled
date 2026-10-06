#include "diagnostic_hooks.h"
#include "ppc_recomp_shared.h"

#include <cstdlib>
#include <iostream>

// SFR_LIVE_TRACE=1: the title's network lobby state machine (82482DB0 and
// the object it runs, state at +76): every state it is set to (82487E80:
// this, state, mode) and every message it shows (82488C08), with the caller.
// States 12..16 end the session (8248A468); 12 shows a message first.
namespace {
bool live_trace() {
    static const bool enabled = [] { const char* t = std::getenv("SFR_LIVE_TRACE"); return t && *t == '1'; }();
    return enabled;
}
}

PPC_FUNC_IMPL(__imp__sub_82487E80);
SFR_CONCURRENT_HOOK(sub_82487E80) {
    sfr::enter_function(ctx, "sub_82487E80", 0x82487E80);
    if (live_trace())
        std::cerr << "LIVE_LOBBY_STATE object=0x" << std::hex << ctx.r3.u32 << " from="
                  << std::dec << sfr::active_memory->load<uint32_t>(ctx.r3.u32 + 76) << " to=" << ctx.r4.u32
                  << " mode=" << ctx.r5.u32 << " lr=0x" << std::hex << ctx.lr << std::dec << '\n';
    __imp__sub_82487E80(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_82488C08);
SFR_CONCURRENT_HOOK(sub_82488C08) {
    sfr::enter_function(ctx, "sub_82488C08", 0x82488C08);
    if (live_trace()) {
        // The network object the lobby reads: [[0x83E53138]+40], its state
        // (+24) and the reason it failed (+1888).
        auto& memory = *sfr::active_memory;
        const uint32_t owner = memory.load<uint32_t>(0x83E53138);
        const uint32_t net = owner ? memory.load<uint32_t>(owner + 40) : 0;
        std::cerr << "LIVE_LOBBY_MESSAGE object=0x" << std::hex << ctx.r3.u32 << std::dec << " r4=" << ctx.r4.u32
                  << " r6=" << ctx.r6.u32 << " r7=" << ctx.r7.u32 << " net_state="
                  << (net ? memory.load<uint32_t>(net + 24) : ~0u) << " net_reason="
                  << (net ? memory.load<uint32_t>(net + 1888) : ~0u) << " lr=0x" << std::hex << ctx.lr << std::dec << '\n';
    }
    __imp__sub_82488C08(ctx, base);
}
