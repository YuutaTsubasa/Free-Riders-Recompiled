#pragma once
#include <cstdint>

// Only the argument/result registers used by nui_race_hooks.cpp. The hook
// regression needs no generated game code or game files.
struct PPCContext {
    union Register { uint64_t u64 = 0; uint32_t u32; } r3, r4, r5;
    uint64_t lr = 0;
};
#define PPC_FUNC(name) void name(PPCContext& ctx, uint8_t* base)
#define PPC_FUNC_IMPL(name) PPC_FUNC(name)
PPC_FUNC(sub_822C8958);
