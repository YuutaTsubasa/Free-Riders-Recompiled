// The title's XAPI heap (statically linked) answered by GuestHeap, as
// Unleashed and Marathon Recompiled replace RtlAllocateHeap and its family
// (their kernel/heap.cpp). Identified by what they call: 824E15E8 enters the
// heap's critical section, grows it with NtAllocateVirtualMemory and raises
// on HEAP_GENERATE_EXCEPTIONS; 824E1EE0 frees and coalesces; 824E21D8 calls
// both; 824E01C8 releases a heap's segments (HeapDestroy's 824DF4A8 returns
// TRUE when it returns NULL); 824E0860 answers HeapSize (824DF4E8, XMemSize's
// heap path). XAllocMem (824DAD90) and XFreeMem (824DAEB0) reach these through
// HeapAlloc/HeapFree; their physical paths stay on MmAllocatePhysicalMemoryEx.
// With SFR_HOST_HEAP=0 every hook runs the original.
#include "diagnostic_hooks.h"
#include "guest_heap.h"
#include "ppc_recomp_shared.h"
#include <iostream>

namespace {
constexpr uint32_t heap_generate_exceptions = 0x4, heap_zero_memory = 0x8, heap_realloc_in_place_only = 0x10;
}

PPC_FUNC_IMPL(__imp__sub_824E15E8);
// RtlAllocateHeap(heap, flags, size)
SFR_CONCURRENT_HOOK(sub_824E15E8) {
    sfr::enter_function(ctx,"sub_824E15E8",0x824E15E8);
    if(!sfr::active_heap) { __imp__sub_824E15E8(ctx,base); return; }
    const uint32_t flags=ctx.r4.u32;
    ctx.r3.u64=sfr::active_heap->allocate(ctx.r3.u32,ctx.r5.u32,flags&heap_zero_memory);
    if(!ctx.r3.u32 && (flags&heap_generate_exceptions))
        throw sfr::RuntimeStop("guest-heap",ctx.r5.u32,"allocation failed with HEAP_GENERATE_EXCEPTIONS");
}

PPC_FUNC_IMPL(__imp__sub_824E1EE0);
// RtlFreeHeap(heap, flags, address) -> BOOLEAN
SFR_CONCURRENT_HOOK(sub_824E1EE0) {
    sfr::enter_function(ctx,"sub_824E1EE0",0x824E1EE0);
    if(!sfr::active_heap) { __imp__sub_824E1EE0(ctx,base); return; }
    const uint32_t address=ctx.r5.u32;
    if(!address) { ctx.r3.u64=1; return; }
    if(sfr::active_heap->free(address)) { ctx.r3.u64=1; return; }
    static uint32_t foreign=0;
    if(foreign++<8)
        std::cerr << "GUEST_HEAP_FOREIGN_FREE heap=0x" << std::hex << ctx.r3.u32 << " address=0x" << address
                  << " lr=0x" << ctx.lr << std::dec << '\n';
    ctx.r3.u64=0;
}

PPC_FUNC_IMPL(__imp__sub_824E21D8);
// RtlReAllocateHeap(heap, flags, address, size)
SFR_CONCURRENT_HOOK(sub_824E21D8) {
    sfr::enter_function(ctx,"sub_824E21D8",0x824E21D8);
    if(!sfr::active_heap) { __imp__sub_824E21D8(ctx,base); return; }
    const uint32_t heap=ctx.r3.u32, flags=ctx.r4.u32, address=ctx.r5.u32, size=ctx.r6.u32;
    if(!address) { ctx.r3.u64=sfr::active_heap->allocate(heap,size,flags&heap_zero_memory); return; }
    ctx.r3.u64=sfr::active_heap->reallocate(heap,address,size,flags&heap_zero_memory,
                                             flags&heap_realloc_in_place_only);
    if(!ctx.r3.u32 && (flags&heap_generate_exceptions))
        throw sfr::RuntimeStop("guest-heap",address,"reallocation failed with HEAP_GENERATE_EXCEPTIONS");
}

PPC_FUNC_IMPL(__imp__sub_824E0860);
// RtlSizeHeap(heap, flags, address) -> size, or ~0 when it is no block
SFR_CONCURRENT_HOOK(sub_824E0860) {
    sfr::enter_function(ctx,"sub_824E0860",0x824E0860);
    if(!sfr::active_heap) { __imp__sub_824E0860(ctx,base); return; }
    ctx.r3.u64=sfr::active_heap->size(ctx.r5.u32);
}

PPC_FUNC_IMPL(__imp__sub_824E01C8);
// RtlDestroyHeap(heap) -> NULL on success: the heap's own segments go as
// before, and with them every block the title allocated from it.
SFR_CONCURRENT_HOOK(sub_824E01C8) {
    sfr::enter_function(ctx,"sub_824E01C8",0x824E01C8);
    if(sfr::active_heap && ctx.r3.u32) sfr::active_heap->destroy(ctx.r3.u32);
    __imp__sub_824E01C8(ctx,base);
}
