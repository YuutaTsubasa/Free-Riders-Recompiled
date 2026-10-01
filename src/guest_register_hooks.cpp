#include "diagnostic_hooks.h"
#include "ppc_recomp_shared.h"
#include "guest_registers.h"

// Named helpers touch only this thread's context and guest memory.
// Checkpoint before the fast path; fallback runs the original body.

PPC_FUNC_IMPL(__imp____savegprlr_14);
SFR_CONCURRENT_HOOK(__savegprlr_14) {
    sfr::enter_function(ctx, "__savegprlr_14", 0x82A56030);
    if (!sfr::try_save_gprs<14>(*sfr::active_memory, ctx))
        __imp____savegprlr_14(ctx, base);
}

PPC_FUNC_IMPL(__imp____restgprlr_14);
SFR_CONCURRENT_HOOK(__restgprlr_14) {
    sfr::enter_function(ctx, "__restgprlr_14", 0x82A56080);
    if (!sfr::try_restore_gprs<14>(*sfr::active_memory, ctx))
        __imp____restgprlr_14(ctx, base);
}

PPC_FUNC_IMPL(__imp____savegprlr_15);
SFR_CONCURRENT_HOOK(__savegprlr_15) {
    sfr::enter_function(ctx, "__savegprlr_15", 0x82A56034);
    if (!sfr::try_save_gprs<15>(*sfr::active_memory, ctx))
        __imp____savegprlr_15(ctx, base);
}

PPC_FUNC_IMPL(__imp____restgprlr_15);
SFR_CONCURRENT_HOOK(__restgprlr_15) {
    sfr::enter_function(ctx, "__restgprlr_15", 0x82A56084);
    if (!sfr::try_restore_gprs<15>(*sfr::active_memory, ctx))
        __imp____restgprlr_15(ctx, base);
}

PPC_FUNC_IMPL(__imp____savegprlr_16);
SFR_CONCURRENT_HOOK(__savegprlr_16) {
    sfr::enter_function(ctx, "__savegprlr_16", 0x82A56038);
    if (!sfr::try_save_gprs<16>(*sfr::active_memory, ctx))
        __imp____savegprlr_16(ctx, base);
}

PPC_FUNC_IMPL(__imp____restgprlr_16);
SFR_CONCURRENT_HOOK(__restgprlr_16) {
    sfr::enter_function(ctx, "__restgprlr_16", 0x82A56088);
    if (!sfr::try_restore_gprs<16>(*sfr::active_memory, ctx))
        __imp____restgprlr_16(ctx, base);
}

PPC_FUNC_IMPL(__imp____savegprlr_17);
SFR_CONCURRENT_HOOK(__savegprlr_17) {
    sfr::enter_function(ctx, "__savegprlr_17", 0x82A5603C);
    if (!sfr::try_save_gprs<17>(*sfr::active_memory, ctx))
        __imp____savegprlr_17(ctx, base);
}

PPC_FUNC_IMPL(__imp____restgprlr_17);
SFR_CONCURRENT_HOOK(__restgprlr_17) {
    sfr::enter_function(ctx, "__restgprlr_17", 0x82A5608C);
    if (!sfr::try_restore_gprs<17>(*sfr::active_memory, ctx))
        __imp____restgprlr_17(ctx, base);
}

PPC_FUNC_IMPL(__imp____savegprlr_18);
SFR_CONCURRENT_HOOK(__savegprlr_18) {
    sfr::enter_function(ctx, "__savegprlr_18", 0x82A56040);
    if (!sfr::try_save_gprs<18>(*sfr::active_memory, ctx))
        __imp____savegprlr_18(ctx, base);
}

PPC_FUNC_IMPL(__imp____restgprlr_18);
SFR_CONCURRENT_HOOK(__restgprlr_18) {
    sfr::enter_function(ctx, "__restgprlr_18", 0x82A56090);
    if (!sfr::try_restore_gprs<18>(*sfr::active_memory, ctx))
        __imp____restgprlr_18(ctx, base);
}

PPC_FUNC_IMPL(__imp____savegprlr_19);
SFR_CONCURRENT_HOOK(__savegprlr_19) {
    sfr::enter_function(ctx, "__savegprlr_19", 0x82A56044);
    if (!sfr::try_save_gprs<19>(*sfr::active_memory, ctx))
        __imp____savegprlr_19(ctx, base);
}

PPC_FUNC_IMPL(__imp____restgprlr_19);
SFR_CONCURRENT_HOOK(__restgprlr_19) {
    sfr::enter_function(ctx, "__restgprlr_19", 0x82A56094);
    if (!sfr::try_restore_gprs<19>(*sfr::active_memory, ctx))
        __imp____restgprlr_19(ctx, base);
}

PPC_FUNC_IMPL(__imp____savegprlr_20);
SFR_CONCURRENT_HOOK(__savegprlr_20) {
    sfr::enter_function(ctx, "__savegprlr_20", 0x82A56048);
    if (!sfr::try_save_gprs<20>(*sfr::active_memory, ctx))
        __imp____savegprlr_20(ctx, base);
}

PPC_FUNC_IMPL(__imp____restgprlr_20);
SFR_CONCURRENT_HOOK(__restgprlr_20) {
    sfr::enter_function(ctx, "__restgprlr_20", 0x82A56098);
    if (!sfr::try_restore_gprs<20>(*sfr::active_memory, ctx))
        __imp____restgprlr_20(ctx, base);
}

PPC_FUNC_IMPL(__imp____savegprlr_21);
SFR_CONCURRENT_HOOK(__savegprlr_21) {
    sfr::enter_function(ctx, "__savegprlr_21", 0x82A5604C);
    if (!sfr::try_save_gprs<21>(*sfr::active_memory, ctx))
        __imp____savegprlr_21(ctx, base);
}

PPC_FUNC_IMPL(__imp____restgprlr_21);
SFR_CONCURRENT_HOOK(__restgprlr_21) {
    sfr::enter_function(ctx, "__restgprlr_21", 0x82A5609C);
    if (!sfr::try_restore_gprs<21>(*sfr::active_memory, ctx))
        __imp____restgprlr_21(ctx, base);
}

PPC_FUNC_IMPL(__imp____savegprlr_22);
SFR_CONCURRENT_HOOK(__savegprlr_22) {
    sfr::enter_function(ctx, "__savegprlr_22", 0x82A56050);
    if (!sfr::try_save_gprs<22>(*sfr::active_memory, ctx))
        __imp____savegprlr_22(ctx, base);
}

PPC_FUNC_IMPL(__imp____restgprlr_22);
SFR_CONCURRENT_HOOK(__restgprlr_22) {
    sfr::enter_function(ctx, "__restgprlr_22", 0x82A560A0);
    if (!sfr::try_restore_gprs<22>(*sfr::active_memory, ctx))
        __imp____restgprlr_22(ctx, base);
}

PPC_FUNC_IMPL(__imp____savegprlr_23);
SFR_CONCURRENT_HOOK(__savegprlr_23) {
    sfr::enter_function(ctx, "__savegprlr_23", 0x82A56054);
    if (!sfr::try_save_gprs<23>(*sfr::active_memory, ctx))
        __imp____savegprlr_23(ctx, base);
}

PPC_FUNC_IMPL(__imp____restgprlr_23);
SFR_CONCURRENT_HOOK(__restgprlr_23) {
    sfr::enter_function(ctx, "__restgprlr_23", 0x82A560A4);
    if (!sfr::try_restore_gprs<23>(*sfr::active_memory, ctx))
        __imp____restgprlr_23(ctx, base);
}

PPC_FUNC_IMPL(__imp____savegprlr_24);
SFR_CONCURRENT_HOOK(__savegprlr_24) {
    sfr::enter_function(ctx, "__savegprlr_24", 0x82A56058);
    if (!sfr::try_save_gprs<24>(*sfr::active_memory, ctx))
        __imp____savegprlr_24(ctx, base);
}

PPC_FUNC_IMPL(__imp____restgprlr_24);
SFR_CONCURRENT_HOOK(__restgprlr_24) {
    sfr::enter_function(ctx, "__restgprlr_24", 0x82A560A8);
    if (!sfr::try_restore_gprs<24>(*sfr::active_memory, ctx))
        __imp____restgprlr_24(ctx, base);
}

PPC_FUNC_IMPL(__imp____savegprlr_25);
SFR_CONCURRENT_HOOK(__savegprlr_25) {
    sfr::enter_function(ctx, "__savegprlr_25", 0x82A5605C);
    if (!sfr::try_save_gprs<25>(*sfr::active_memory, ctx))
        __imp____savegprlr_25(ctx, base);
}

PPC_FUNC_IMPL(__imp____restgprlr_25);
SFR_CONCURRENT_HOOK(__restgprlr_25) {
    sfr::enter_function(ctx, "__restgprlr_25", 0x82A560AC);
    if (!sfr::try_restore_gprs<25>(*sfr::active_memory, ctx))
        __imp____restgprlr_25(ctx, base);
}

PPC_FUNC_IMPL(__imp____savegprlr_26);
SFR_CONCURRENT_HOOK(__savegprlr_26) {
    sfr::enter_function(ctx, "__savegprlr_26", 0x82A56060);
    if (!sfr::try_save_gprs<26>(*sfr::active_memory, ctx))
        __imp____savegprlr_26(ctx, base);
}

PPC_FUNC_IMPL(__imp____restgprlr_26);
SFR_CONCURRENT_HOOK(__restgprlr_26) {
    sfr::enter_function(ctx, "__restgprlr_26", 0x82A560B0);
    if (!sfr::try_restore_gprs<26>(*sfr::active_memory, ctx))
        __imp____restgprlr_26(ctx, base);
}

PPC_FUNC_IMPL(__imp____savegprlr_27);
SFR_CONCURRENT_HOOK(__savegprlr_27) {
    sfr::enter_function(ctx, "__savegprlr_27", 0x82A56064);
    if (!sfr::try_save_gprs<27>(*sfr::active_memory, ctx))
        __imp____savegprlr_27(ctx, base);
}

PPC_FUNC_IMPL(__imp____restgprlr_27);
SFR_CONCURRENT_HOOK(__restgprlr_27) {
    sfr::enter_function(ctx, "__restgprlr_27", 0x82A560B4);
    if (!sfr::try_restore_gprs<27>(*sfr::active_memory, ctx))
        __imp____restgprlr_27(ctx, base);
}

PPC_FUNC_IMPL(__imp____savegprlr_28);
SFR_CONCURRENT_HOOK(__savegprlr_28) {
    sfr::enter_function(ctx, "__savegprlr_28", 0x82A56068);
    if (!sfr::try_save_gprs<28>(*sfr::active_memory, ctx))
        __imp____savegprlr_28(ctx, base);
}

PPC_FUNC_IMPL(__imp____restgprlr_28);
SFR_CONCURRENT_HOOK(__restgprlr_28) {
    sfr::enter_function(ctx, "__restgprlr_28", 0x82A560B8);
    if (!sfr::try_restore_gprs<28>(*sfr::active_memory, ctx))
        __imp____restgprlr_28(ctx, base);
}

PPC_FUNC_IMPL(__imp____savegprlr_29);
SFR_CONCURRENT_HOOK(__savegprlr_29) {
    sfr::enter_function(ctx, "__savegprlr_29", 0x82A5606C);
    if (!sfr::try_save_gprs<29>(*sfr::active_memory, ctx))
        __imp____savegprlr_29(ctx, base);
}

PPC_FUNC_IMPL(__imp____restgprlr_29);
SFR_CONCURRENT_HOOK(__restgprlr_29) {
    sfr::enter_function(ctx, "__restgprlr_29", 0x82A560BC);
    if (!sfr::try_restore_gprs<29>(*sfr::active_memory, ctx))
        __imp____restgprlr_29(ctx, base);
}

PPC_FUNC_IMPL(__imp____savegprlr_30);
SFR_CONCURRENT_HOOK(__savegprlr_30) {
    sfr::enter_function(ctx, "__savegprlr_30", 0x82A56070);
    if (!sfr::try_save_gprs<30>(*sfr::active_memory, ctx))
        __imp____savegprlr_30(ctx, base);
}

PPC_FUNC_IMPL(__imp____restgprlr_30);
SFR_CONCURRENT_HOOK(__restgprlr_30) {
    sfr::enter_function(ctx, "__restgprlr_30", 0x82A560C0);
    if (!sfr::try_restore_gprs<30>(*sfr::active_memory, ctx))
        __imp____restgprlr_30(ctx, base);
}

PPC_FUNC_IMPL(__imp____savegprlr_31);
SFR_CONCURRENT_HOOK(__savegprlr_31) {
    sfr::enter_function(ctx, "__savegprlr_31", 0x82A56074);
    if (!sfr::try_save_gprs<31>(*sfr::active_memory, ctx))
        __imp____savegprlr_31(ctx, base);
}

PPC_FUNC_IMPL(__imp____restgprlr_31);
SFR_CONCURRENT_HOOK(__restgprlr_31) {
    sfr::enter_function(ctx, "__restgprlr_31", 0x82A560C4);
    if (!sfr::try_restore_gprs<31>(*sfr::active_memory, ctx))
        __imp____restgprlr_31(ctx, base);
}
