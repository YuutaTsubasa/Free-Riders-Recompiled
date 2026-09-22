#include "ppc_recomp_shared.h"
#include "diagnostic_hooks.h"
#include "guest_memory.h"
#include "native_input.h"
#include <cstdlib>
#include <iostream>
#include <unordered_map>

// Compatibility patches for latent bugs in the original game code. Each one
// is narrow, documented and leaves the original body in charge.

PPC_FUNC_IMPL(__imp__sub_82437F38);

// Camera image object constructor (84 bytes from the game allocator). It
// opens a NUI image stream (82768C40) and only when that succeeds stores four
// textures at +12..+24. Its destructor (82438198) releases each nonzero slot,
// so with no Kinect the stale heap contents there are passed to the D3D
// release (824F1F50), which decrements an arbitrary word. Clear the four
// slots first, as freshly committed memory would be, then run the original.
SFR_HOOK(sub_82437F38) {
    sfr::enter_function(ctx,"sub_82437F38",0x82437F38);
    const uint32_t object=ctx.r3.u32;
    sfr::active_memory->check_write(uint64_t(object)+12,16);
    for(uint32_t offset=12;offset<28;offset+=4)
        sfr::active_memory->store<uint32_t>(uint64_t(object)+offset,0);
    __imp__sub_82437F38(ctx,base);
    std::cerr << "GAME_PATCH camera_image_slots object=0x" << std::hex << object
              << " initialized=" << std::dec << int(sfr::active_memory->load<uint8_t>(uint64_t(object)+77)) << '\n';
}

PPC_FUNC_IMPL(__imp__sub_82817B48);

// Movie frame: the game calls the XMV player's RenderNextFrame (vtable +80)
// every frame with flag bit 0 (return at once when no decoded frame is
// queued). On the console the decoder thread has its own core and a frame is
// always ready; here all guest threads share one permit, the decoder falls
// behind and two thirds of the frames were presented without the movie
// (black). Without the flag the player waits on its frame/end/error events
// (828266C8), which also gives the decoder the permit.
//
// Skipping: A, B, START or BACK on the pad (or SFR_SKIP_MOVIES=1) ends the
// movie as the player's end of file does (XMV_ENDOFFILE, which the callers
// 8243A728 and 8243A968 test), since the decoder is still slower than real
// time.
SFR_HOOK(sub_82817B48) {
    sfr::enter_function(ctx,"sub_82817B48",0x82817B48);
    // A movie ended before its first frames leaves the title waiting on a
    // white screen, so SFR_SKIP_MOVIES lets each player show 30 frames.
    static const bool skip_all=std::getenv("SFR_SKIP_MOVIES")!=nullptr;
    static std::unordered_map<uint32_t,uint32_t> frames;
    const bool started=++frames[ctx.r3.u32]>30;
    constexpr uint16_t skip_buttons=sfr::gamepad_button::a|sfr::gamepad_button::b|sfr::gamepad_button::start|
                                    sfr::gamepad_button::back;
    if(started && (skip_all || (sfr::nui_gamepad().buttons & skip_buttons))) {
        static uint32_t skipped=0;
        if(skipped++<8) std::cerr << "GAME_PATCH movie_skip player=0x" << std::hex << ctx.r3.u32 << std::dec << '\n';
        ctx.r3.u64=0x16660026u;  // XMV_ENDOFFILE
        return;
    }
    ctx.r4.u32&=~1u;
    __imp__sub_82817B48(ctx,base);
}

PPC_FUNC_IMPL(__imp__sub_824D0B10);

// Work-sharing job 823B5D40 (run by the job system each race frame) queues
// its items, resumes three helper threads (823B60C0, on processors 1, 4 and
// 5), drains the queue itself too, then waits for the helpers' done events
// with WaitForMultipleObjects(3, events, TRUE, 16 ms) and ignores the
// result: it resets and requeues the items whether or not the helpers have
// finished. On the console they always have. Here a helper can still be in
// an item's callback after 16 ms (slower code, waits for the execution
// permit), and a race start then ran a reset item's callback: the pure
// virtual call R6025. The wait stays finite - a helper that signals done but
// suspends itself only after the next frame's resume is released by the
// next frame's wait - but gets SFR_WORK_SHARE_WAIT_MS (default 1000) instead
// of 16. Timeouts are reported.
SFR_CONCURRENT_HOOK(sub_824D0B10) {
    sfr::enter_function(ctx,"sub_824D0B10",0x824D0B10);
    const bool work_share=ctx.lr==0x823B5EA0 && ctx.r3.u32==3 && ctx.r5.u32==1 && ctx.r6.u32==16;
    if(work_share) {
        static const uint32_t wait_ms=[]{
            const char* text=std::getenv("SFR_WORK_SHARE_WAIT_MS");
            const long value=text?std::strtol(text,nullptr,10):1000;
            return uint32_t(value>0?value:1000);
        }();
        ctx.r6.u64=wait_ms;
    }
    __imp__sub_824D0B10(ctx,base);
    if(work_share && ctx.r3.u32==0x102) {
        static std::atomic<uint32_t> timeouts{0};
        if(timeouts++<16) std::cerr << "GAME_PATCH work_share_wait_timeout count=" << timeouts << '\n';
    }
}
