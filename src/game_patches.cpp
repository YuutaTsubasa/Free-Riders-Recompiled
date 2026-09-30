#include "ppc_recomp_shared.h"
#include "diagnostic_hooks.h"
#include "wait_trace.h"
#include "guest_memory.h"
#include "native_input.h"
#include "avatar_state.h"
#include "avatar_transform.h"
#include "avatar_clip_pose.h"
#include <algorithm>
#include <bit>
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
// queued). This compatibility patch predates the per-core scheduler: when
// the decoder fell behind, presents without a movie frame appeared black.
// Without the flag the player waits for decoder readiness (828266C8),
// releasing its execution permit while blocked. Keep that behavior while
// measuring the current decoder rather than assuming the old bottleneck.
//
// Skipping: A, B, START or BACK on the pad (or SFR_SKIP_MOVIES=1) ends the
// movie as the player's end of file does (XMV_ENDOFFILE, which the callers
// 8243A728 and 8243A968 test). Decoder throughput depends on the host.
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
    struct Progress {
        uint32_t rendered=0, trace_calls=0, trace_success=0;
        uint64_t trace_total_calls=0;
        uint32_t trace_last_status=0;
        double trace_ms=0, trace_max_ms=0;
        std::chrono::steady_clock::time_point first{}, trace_reported{};
    };
    static std::unordered_map<uint32_t,Progress> players;
    const uint32_t player=ctx.r3.u32;
    auto& progress=players[player];
    const auto now=std::chrono::steady_clock::now();
    if(progress.first==std::chrono::steady_clock::time_point{}) progress.first=now;
    static const bool trace_movie=[]{ const char* t=std::getenv("SFR_WAIT_TRACE"); return t && *t=='1'; }();
    if(trace_movie && progress.trace_reported==std::chrono::steady_clock::time_point{}) progress.trace_reported=now;
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
    const uint32_t input_flags=ctx.r4.u32, caller=uint32_t(ctx.lr);
    ctx.r4.u32&=~1u;
    const auto call_started=trace_movie?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
    __imp__sub_82817B48(ctx,base);
    if(ctx.r3.u32==0) ++progress.rendered;  // retain the existing success-based skip policy
    if(trace_movie) {
        const auto ended=std::chrono::steady_clock::now();
        const double ms=std::chrono::duration<double,std::milli>(ended-call_started).count();
        ++progress.trace_calls;
        ++progress.trace_total_calls;
        progress.trace_success+=ctx.r3.u32==0;
        progress.trace_ms+=ms;
        progress.trace_max_ms=std::max(progress.trace_max_ms,ms);
        if(progress.trace_total_calls==1 || ctx.r3.u32!=progress.trace_last_status ||
           progress.trace_calls>=30 || ended-progress.trace_reported>=std::chrono::seconds(2)) {
            std::cerr << "MOVIE_TRACE player=0x" << std::hex << player << " last_status=0x" << ctx.r3.u32 << std::dec
                << " frame=" << sfr::present_count.load() << " host_ms="
                << std::chrono::duration_cast<std::chrono::milliseconds>(ended.time_since_epoch()).count()
                << " window_ms=" << std::chrono::duration<double,std::milli>(ended-progress.trace_reported).count()
                << " calls=" << progress.trace_calls << " success_returns=" << progress.trace_success
                << " call_ms=" << progress.trace_ms << " max_call_ms=" << progress.trace_max_ms
                << " total_calls=" << progress.trace_total_calls << " success_total=" << progress.rendered
                << " elapsed_ms=" << std::chrono::duration<double,std::milli>(ended-progress.first).count()
                << " caller=0x" << std::hex << caller << " input_flags=0x" << input_flags << std::dec << '\n';
            progress.trace_calls=progress.trace_success=0;
            progress.trace_ms=progress.trace_max_ms=0;
            progress.trace_reported=ended;
        }
        progress.trace_last_status=ctx.r3.u32;
    }
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
            sfr::traced_host_wait([](void*){ std::this_thread::sleep_for(std::chrono::microseconds(50)); },nullptr,"poll_50us",50000);
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
            sfr::traced_host_wait([](void*){ std::this_thread::sleep_for(std::chrono::microseconds(100)); },nullptr,"poll_100us",100000);
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

// The avatar's parts. Six functions walk the same structure, which the
// title keeps at [avatar+0x2FC40]: the buffer XamAvatarGetAssets filled, read
// as a pointer to a count of parts. Each of them forms that address from r3.
//
// Without a real avatar system there are no assets, so for some of these
// objects the first link is null and the walk reads address zero, stopping
// the run (docs/avatar.md). An avatar with no parts has nothing for them to
// do, so they are skipped -- the character stays in the race with nothing
// drawn for it, which is what an empty avatar is.
//
// 823B9B50 is left alone: it is the one that builds the structure.
namespace {
bool avatar_has_parts(uint32_t avatar) {
    if(!sfr::active_memory || !avatar) return true;
    try {
        const uint32_t list=sfr::active_memory->load<uint32_t>(uint64_t(avatar)+0x2FC40);
        if(!list) return false;
        const uint32_t entry=sfr::active_memory->load<uint32_t>(list);
        if(!entry) return false;
        return sfr::active_memory->load<uint32_t>(entry)!=0;
    } catch(...) { return true; }  // unreadable is not ours to judge
}

void note_avatar_skip(uint32_t address) {
    static std::atomic<uint64_t> skipped{0};
    const uint64_t count=skipped.fetch_add(1,std::memory_order_relaxed)+1;
    if((count&(count-1))==0)
        std::cerr << "GAME_PATCH avatar_without_parts function=0x" << std::hex << address << std::dec
                  << " skipped=" << count << char(10);
}
}

#define SFR_AVATAR_PART_WALK(address) \
    PPC_FUNC_IMPL(__imp__sub_##address); \
    SFR_HOOK(sub_##address) { \
        sfr::enter_function(ctx,"sub_" #address,0x##address); \
        if(!avatar_has_parts(ctx.r3.u32)) { note_avatar_skip(0x##address); return; } \
        __imp__sub_##address(ctx,base); \
    }

PPC_FUNC_IMPL(__imp__sub_823B9D60);
SFR_HOOK(sub_823B9D60) {
    sfr::enter_function(ctx, "sub_823B9D60", 0x823B9D60);
    if (!avatar_has_parts(ctx.r3.u32)) {
        const uint32_t avatar = ctx.r3.u32;
        auto& memory = *sfr::active_memory;
        const uint32_t header = memory.load<uint32_t>(uint64_t(avatar) + 0x2FC40);
        // A real zero-count header is safe for the original routine. Keep its
        // overlapped reset and body-size classification after the empty loop.
        if (!header || !memory.readable(header, 12) || memory.load<uint32_t>(uint64_t(header) + 8) != 0) {
            note_avatar_skip(0x823B9D60);
            // 823B9D80 clears seven words before touching the parts header;
            // 823B9E6C returns true even when the parts count is zero.
            for (uint32_t offset = 12; offset <= 36; offset += 4)
                memory.store<uint32_t>(uint64_t(avatar) + offset, 0);
            ctx.r3.u64 = 1;
            return;
        }
    }
    __imp__sub_823B9D60(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_823B9F00);
SFR_HOOK(sub_823B9F00) {
    sfr::enter_function(ctx, "sub_823B9F00", 0x823B9F00);
    if (!avatar_has_parts(ctx.r3.u32)) {
        note_avatar_skip(0x823B9F00);
        ctx.r3.u64 = 1;  // the empty-loop return at 823BA01C
        return;
    }
    __imp__sub_823B9F00(ctx, base);
}

SFR_AVATAR_PART_WALK(823BA028)
SFR_AVATAR_PART_WALK(823BA468)
SFR_AVATAR_PART_WALK(823BAFE0)
SFR_AVATAR_PART_WALK(823BB2E8)

PPC_FUNC_IMPL(__imp__sub_823B97A8);
namespace {
std::array<sfr::AvatarClipPose, 2> avatar_clips;
sfr::AvatarPose read_avatar_pose(const sfr::GuestMemory& memory, uint32_t animation) {
    sfr::AvatarPose pose;
    if (!animation || !memory.readable(uint64_t(animation) + 11556, 4)) return pose;
    const uint32_t current = memory.load<uint32_t>(uint64_t(animation) + 11556);
    const uint64_t raw = uint64_t(animation) + 16 + 3456ull * current;
    if (current >= 2 || !memory.readable(raw, 3456)) return pose;
    pose.valid = true;
    for (uint32_t bone = 0; bone < pose.bones.size(); ++bone) {
        for (uint32_t i = 0; i < 3; ++i)
            pose.bones[bone].translation[i] = std::bit_cast<float>(memory.load<uint32_t>(raw + bone * 48 + i * 4));
        for (uint32_t i = 0; i < 4; ++i)
            pose.bones[bone].rotation[i] = std::bit_cast<float>(memory.load<uint32_t>(raw + bone * 48 + 16 + i * 4));
    }
    return pose;
}
}
PPC_FUNC_IMPL(__imp__sub_823BBCB8);
SFR_HOOK(sub_823BBCB8) {
    sfr::enter_function(ctx, "sub_823BBCB8", 0x823BBCB8);
    const uint32_t controller = ctx.r3.u32, renderer = ctx.r4.u32, animation = ctx.r5.u32;
    __imp__sub_823BBCB8(ctx, base);
    if (!sfr::active_memory) return;
    const auto& memory = *sfr::active_memory;
    const auto owner = sfr::local_avatar_for_animation(memory, controller, renderer, animation);
    if (!owner) return;
    // Preserve evaluated/blended clip output before tracked shoulder corrections.
    avatar_clips[owner->local_slot] = {owner->rider, animation, sfr::present_count.load(), read_avatar_pose(memory, animation), owner->manager, renderer, controller};
}
PPC_FUNC_IMPL(__imp__sub_823BC330);
SFR_HOOK(sub_823BC330) {
    sfr::enter_function(ctx, "sub_823BC330", 0x823BC330);
    // Only the hand attachment getter used by 8227FD58. Gameplay IK and
    // all other native palette consumers retain the original SDK matrices.
    if (ctx.lr == 0x8227FDA4 && (ctx.r4.u32 == 33 || ctx.r4.u32 == 36) && sfr::active_memory) {
        auto& memory = *sfr::active_memory;
        const auto owner = sfr::local_avatar_for_animation(memory, ctx.r3.u32);
        if (owner && memory.readable(ctx.r5.u32, 64)) {
            auto pose = avatar_clips[owner->local_slot].current(owner->rider, owner->animation, sfr::present_count.load(), owner->manager, owner->renderer, owner->controller);
            if (pose) {
                pose->mirrored = memory.load<uint8_t>(uint64_t(owner->renderer) + 8) != 0;
                if (const auto hand = sfr::avatar_hand_transform(*pose, ctx.r4.u32)) {
                    for (uint32_t i = 0; i < 16; ++i)
                        memory.store<uint32_t>(uint64_t(ctx.r5.u32) + i * 4, std::bit_cast<uint32_t>((*hand)[i]));
                    static const bool trace = std::getenv("SFR_TRACE_AVATAR") != nullptr;
                    static uint32_t traced = 0;
                    if (trace && traced++ % 120 == 0)
                        std::cerr << "AVATAR_HAND present=" << sfr::present_count.load() << " player=" << owner->local_slot
                                  << " bone=" << ctx.r4.u32 << " xyz=" << (*hand)[12] << ',' << (*hand)[13] << ',' << (*hand)[14]
                                  << " render_offset=" << std::bit_cast<float>(memory.load<uint32_t>(uint64_t(owner->rider)+208)) << ','
                                  << std::bit_cast<float>(memory.load<uint32_t>(uint64_t(owner->rider)+212)) << ','
                                  << std::bit_cast<float>(memory.load<uint32_t>(uint64_t(owner->rider)+216)) << '\n';
                    return;
                }
            }
        }
    }
    __imp__sub_823BC330(ctx, base);
}
SFR_HOOK(sub_823B97A8) {
    sfr::enter_function(ctx, "sub_823B97A8", 0x823B97A8);
    // A scene camera's Avatar draw (the other caller is the shadow pass).
    // r5/r6/r7 are row-major world, view, projection. Copy before the guest
    // returns: these point into temporary draw state, not permanent storage.
    if (ctx.lr == 0x822AB04C && ctx.r9.u32 < 2 && sfr::active_memory) {
        const auto& memory = *sfr::active_memory;
        const auto owner = sfr::local_avatar_for_renderer(memory, ctx.r3.u32);
        if (owner && ctx.r9.u32 < owner->local_count) {
            const uint32_t rider = owner->rider, character = owner->character;
            if (memory.readable(ctx.r5.u32, 64) && memory.readable(ctx.r6.u32, 64) && memory.readable(ctx.r7.u32, 64)) {
                sfr::AvatarFrameTransform frame;
                for (uint32_t i = 0; i < 16; ++i) {
                    frame.world[i] = std::bit_cast<float>(memory.load<uint32_t>(uint64_t(ctx.r5.u32) + i * 4));
                    frame.view[i] = std::bit_cast<float>(memory.load<uint32_t>(uint64_t(ctx.r6.u32) + i * 4));
                    frame.projection[i] = std::bit_cast<float>(memory.load<uint32_t>(uint64_t(ctx.r7.u32) + i * 4));
                }
                if (auto clip = avatar_clips[owner->local_slot].current(rider, ctx.r4.u32, sfr::present_count.load(), owner->manager, owner->renderer, owner->controller)) frame.pose = *clip;
                // BA028 reflects the completed skeleton across X for goofy.
                if (memory.readable(uint64_t(ctx.r3.u32) + 8, 1))
                    frame.pose.mirrored = memory.load<uint8_t>(uint64_t(ctx.r3.u32) + 8) != 0;
                frame.present = sfr::present_count.load();
                sfr::draw_avatar_model(frame);
                static const bool trace_pose = std::getenv("SFR_TRACE_AVATAR_POSE") != nullptr;
                static const unsigned pose_every = [] {
                    const char* text = std::getenv("SFR_TRACE_AVATAR_POSE_EVERY");
                    const unsigned value = text ? unsigned(std::strtoul(text, nullptr, 10)) : 120u;
                    return value ? value : 120u;
                }();
                static uint32_t pose_frames = 0;
                if (trace_pose && pose_frames++ % pose_every == 0 && memory.readable(uint64_t(ctx.r4.u32) + 11556, 4)) {
                    const uint32_t current = memory.load<uint32_t>(uint64_t(ctx.r4.u32) + 11556);
                    const uint64_t raw = uint64_t(ctx.r4.u32) + 16 + 3456ull * current;
                    const uint32_t controller = memory.readable(uint64_t(character) + 12, 4)
                        ? memory.load<uint32_t>(uint64_t(character) + 12) : 0;
                    std::ostringstream dump;
                    dump << "AVATAR_POSE frame=" << pose_frames << " present=" << sfr::present_count.load() << " buffer=" << current
                         << " clip_valid=" << frame.pose.valid << " mirrored=" << frame.pose.mirrored
                         << " clip_chest=" << frame.pose.bones[5].rotation[1] << " clip_root_y=" << frame.pose.bones[0].translation[1];
                    if (controller && memory.readable(controller, 152)) {
                        const uint32_t clip = memory.load<uint32_t>(uint64_t(controller) + 136);
                        dump << " bones=" << memory.load<uint32_t>(uint64_t(controller) + 148) << " clip=0x" << std::hex << clip;
                        if (clip && memory.readable(clip, 4)) {
                            const uint32_t vt = memory.load<uint32_t>(clip);
                            if (vt && memory.readable(vt, 28)) dump << " eval=0x" << memory.load<uint32_t>(uint64_t(vt) + 24);
                        }
                        dump << std::dec;
                    }
                    if (current < 2 && memory.readable(raw, 3456))
                        for (uint32_t bone = 0; bone < 72; ++bone) {
                            dump << " bone" << bone << '=';
                            for (uint32_t word = 0; word < 12; ++word)
                                dump << (word ? "," : "") << std::bit_cast<float>(memory.load<uint32_t>(raw + bone * 48 + word * 4));
                        }
                    std::cerr << dump.str() << '\n';
                }
            }
        }
    }
    static const bool trace = [] {
        const char* text = std::getenv("SFR_TRACE_AVATAR");
        return text && *text && *text != '0';
    }();
    static unsigned traced = 0;
    if (trace && traced < 4 && sfr::active_memory) {
        const auto& memory = *sfr::active_memory;
        const uint32_t rider = sfr::single_player_avatar_racer(memory);
        if (rider) {
            ++traced;
            std::ostringstream text;
            text << "AVATAR_MATRICES object=0x" << std::hex << ctx.r3.u32
                 << " rider=0x" << rider << " lr=0x" << ctx.lr << std::dec
                 << " camera=" << ctx.r9.u32;
            for (const auto pointer : {ctx.r5.u32, ctx.r6.u32, ctx.r7.u32}) {
                text << " matrix=";
                if (memory.readable(pointer, 64))
                    for (uint32_t i = 0; i < 16; ++i)
                        text << (i ? "," : "") << std::bit_cast<float>(memory.load<uint32_t>(uint64_t(pointer) + i * 4));
                else text << "unreadable";
            }
            std::cerr << text.str() << '\n';
        }
    }
    __imp__sub_823B97A8(ctx, base);
}
