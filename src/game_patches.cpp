#include "ppc_recomp_shared.h"
#include "diagnostic_hooks.h"
#include "guest_memory.h"
#include "native_input.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <thread>
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
    // An empty value is off, like every other switch: the launcher writes the
    // name with no value when the setting is turned off, and a host that keeps
    // it in the environment that way (Android) must not read it as on.
    static const bool skip_all=[]{ const char* t=std::getenv("SFR_SKIP_MOVIES"); return t && *t && *t!='0'; }();
    // Thirty frames the player really rendered, not thirty calls: on a phone
    // the decoder can answer every call with nothing for a while, and ending
    // the movie then left the title on the white screen this allowance is
    // meant to avoid. A player that renders nothing at all still has to end,
    // so a wall-clock allowance runs beside it.
    struct Progress { uint32_t rendered=0; std::chrono::steady_clock::time_point first{}; };
    static std::unordered_map<uint32_t,Progress> players;
    const uint32_t player=ctx.r3.u32;
    auto& progress=players[player];
    const auto now=std::chrono::steady_clock::now();
    if(progress.first==std::chrono::steady_clock::time_point{}) progress.first=now;
    static const uint32_t allowance_ms=[]{
        const char* t=std::getenv("SFR_MOVIE_ALLOWANCE_MS");
        const long value=t?std::strtol(t,nullptr,10):10000;
        return uint32_t(value>0?value:10000);
    }();
    const bool started=progress.rendered>30 ||
        now-progress.first>std::chrono::milliseconds(allowance_ms);
    constexpr uint16_t skip_buttons=sfr::gamepad_button::a|sfr::gamepad_button::b|sfr::gamepad_button::start|
                                    sfr::gamepad_button::back;
    if(started && (skip_all || (sfr::nui_gamepad().buttons & skip_buttons))) {
        static uint32_t skipped=0;
        if(skipped++<8)
            std::cerr << "GAME_PATCH movie_skip player=0x" << std::hex << player << std::dec
                      << " rendered=" << progress.rendered << '\n';
        ctx.r3.u64=0x16660026u;  // XMV_ENDOFFILE
        return;
    }
    ctx.r4.u32&=~1u;
    __imp__sub_82817B48(ctx,base);
    if(ctx.r3.u32==0) ++progress.rendered;  // a frame the player really drew
}

// How many of the dispatcher's helper threads (823B60C0) may still be inside
// a job's callback: set when a helper takes a job, cleared when it sets its
// done event at 823B614C, which it does only once the queue is empty and its
// last callback has returned. A helper that takes another job after that
// (the loop after its self-suspend) counts again, which is exactly the case
// the dispatcher must not reset jobs under.
namespace {
std::atomic<uint32_t> helpers_in_jobs{0};
thread_local bool helper_in_job=false;

void helper_leaves_job() {
    if(!helper_in_job) return;
    helper_in_job=false;
    helpers_in_jobs.fetch_sub(1,std::memory_order_acq_rel);
}
}

PPC_FUNC_IMPL(__imp__sub_824D0B18);
namespace sfr { void prepare_worker_self_suspend(); }

// SetEvent. The helper's own (return address 823B6150) says it has finished
// the jobs it took.
SFR_CONCURRENT_HOOK(sub_824D0B18) {
    sfr::enter_function(ctx,"sub_824D0B18",0x824D0B18);
    if(ctx.lr==0x823B6150) helper_leaves_job();
    // Worker 824C39C8 signals done and then suspends itself. A phone can run
    // the main thread's next ResumeThread between those two calls, losing
    // that resume and hanging its next completion wait (824C3A98). Register
    // the suspension before publishing done; its following self-suspend
    // consumes that registration instead of incrementing the count again.
    if(ctx.lr==0x824C3A3C) {
        static const bool handoff=[]{
            const char* t=std::getenv("SFR_COMPLETION_SUSPEND_HANDOFF");
            return !t || *t!='0';
        }();
        if(handoff) sfr::prepare_worker_self_suspend();
    }
    __imp__sub_824D0B18(ctx,base);
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
    // The three done events say the helpers found the queue empty, but not
    // that they are out of the job they took last: the caller resets and
    // reuses every job as soon as this returns, and a helper still inside one
    // then reads a job whose 176..496 have been zeroed (a null dereference in
    // the callback) or, next time round, a function word that is the base
    // class's pure slot. Hold the reset back until they are out.
    if(work_share && helpers_in_jobs.load(std::memory_order_acquire)) {
        static const uint32_t wait_ms=[]{
            const char* text=std::getenv("SFR_JOB_DRAIN_WAIT_MS");
            const long value=text?std::strtol(text,nullptr,10):20;
            return uint32_t(value>=0?value:20);
        }();
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(wait_ms);
        while(helpers_in_jobs.load(std::memory_order_acquire) && std::chrono::steady_clock::now()<deadline)
            sfr::wait_without_permit([](void*){ std::this_thread::sleep_for(std::chrono::microseconds(50)); },nullptr);
        static std::atomic<uint32_t> waits{0}, left{0};
        const uint32_t count=++waits;
        if(helpers_in_jobs.load(std::memory_order_acquire)) ++left;
        if(count<=8 || count%256==0)
            std::cerr << "GAME_PATCH job_drain_wait count=" << count << " still_running=" << left << '\n';
    }
}

PPC_FUNC_IMPL(__imp__sub_82A53BC0);

// The CRT's pure virtual call handler (R6025), which the title turns into a
// KeBugCheck. The job dispatcher 823B5D40 and its helper 823B60C0 pop a job
// from the queue and call the function word at job+168 (with job+64 and the
// word at job+520). Both test that word against zero first, so reaching the
// pure handler means the word held the base class's pure slot: the job was
// being built, or - what a run with SFR_THREAD_START_DELAY_US=0 shows - had
// already been taken apart, since 500 ms later the word is zero. The
// dispatcher resets and reuses its jobs once its wait elapses whether or not
// the helpers have finished (see the 824D0B10 patch above), and a helper
// resumed late is still draining the queue then.
//
// So: wait briefly in case the job is only half built (without holding the
// execution permit, so the builder can finish), and otherwise skip the job
// the way its owner already assumes it is finished, instead of stopping the
// whole game. This is what stopped about one Grand Prix load in four; the
// 2 ms SFR_THREAD_START_DELAY_US hid it on this machine, but no fixed delay
// can be right for every host, and a phone is much slower.
SFR_HOOK(sub_82A53BC0) {
    sfr::enter_function(ctx,"sub_82A53BC0",0x82A53BC0);
    // Only the two job-dispatch call sites; any other pure call is a real one.
    if((ctx.lr==0x823B5E88 || ctx.lr==0x823B6144) && sfr::active_memory) {
        static const uint32_t wait_ms=[]{
            const char* text=std::getenv("SFR_JOB_READY_WAIT_MS");
            const long value=text?std::strtol(text,nullptr,10):20;
            return uint32_t(value>0?value:20);
        }();
        const uint32_t job=ctx.r3.u32;
        uint32_t call=0;
        for(uint32_t attempt=0;attempt<wait_ms*10;++attempt) {
            call=sfr::active_memory->load<uint32_t>(uint64_t(job)+168);
            if(call && call!=0x82A53BC0) break;
            call=0;
            sfr::wait_without_permit([](void*){ std::this_thread::sleep_for(std::chrono::microseconds(100)); },nullptr);
        }
        static std::atomic<uint32_t> late{0}, skipped{0};
        const uint32_t count=call?++late:++skipped;
        if(count<=8) {
            std::ostringstream text;
            text << "GAME_PATCH " << (call?"job_filled_in_late":"job_skipped_after_reset")
                 << " job=0x" << std::hex << job << " call=0x" << call << " lr=0x" << ctx.lr << " words=";
            try {
                for(uint32_t offset : {160u,164u,168u,172u,176u,180u,184u,492u,496u,516u,520u})
                    text << std::dec << offset << ':' << std::hex
                         << sfr::active_memory->load<uint32_t>(uint64_t(job)+offset) << ' ';
            } catch(...) { text << "unreadable"; }
            std::cerr << text.str() << std::dec << " count=" << count << '\n';
        }
        if(call) ctx.ctr.u64=call, PPC_CALL_INDIRECT_FUNC(call);
        return;
    }
    __imp__sub_82A53BC0(ctx,base);
}

PPC_FUNC_IMPL(__imp__sub_82750C40);

// The lock-free queue's take. Many owners share it; only the dispatcher's own
// take (return address 823B5E58) and its helpers' (823B6114) are about jobs.
// SFR_JOB_TRACE=1 prints what a caller then tests.
SFR_CONCURRENT_HOOK(sub_82750C40) {
    sfr::enter_function(ctx,"sub_82750C40",0x82750C40);
    static const bool trace=[]{ const char* t=std::getenv("SFR_JOB_TRACE"); return t && *t=='1'; }();
    const uint32_t slot=ctx.r4.u32, caller=uint32_t(ctx.lr);
    __imp__sub_82750C40(ctx,base);
    if(!sfr::active_memory || !slot || (caller!=0x823B5E58 && caller!=0x823B6114)) return;
    uint32_t job=0;
    try { job=sfr::active_memory->load<uint32_t>(slot); } catch(...) { return; }
    if(caller==0x823B6114 && job!=0 && !helper_in_job) {
        helper_in_job=true;
        helpers_in_jobs.fetch_add(1,std::memory_order_acq_rel);
    } else if(caller==0x823B6114 && job==0) {
        helper_leaves_job();
    }
    if(!trace || !job) return;
    static std::atomic<uint32_t> traced{0};
    if(traced>=64) return;
    ++traced;
    try {
        std::ostringstream text;
        text << "JOB_TAKE lr=0x" << std::hex << caller << " job=0x" << job << " words=";
        for(uint32_t offset : {160u,164u,168u,172u,176u,180u,184u,492u,496u,516u,520u})
            text << offset << ':' << sfr::active_memory->load<uint32_t>(uint64_t(job)+offset) << ' ';
        std::cerr << text.str() << std::dec << '\n';
    } catch(...) {}
}
