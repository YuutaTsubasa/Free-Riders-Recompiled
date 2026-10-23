#include "ppc_recomp_shared.h"
#include "diagnostic_hooks.h"
#include "wait_trace.h"
#include "guest_memory.h"
#include "guest_graphics.h"
#include "aspect_settings.h"
#include "native_input.h"
#include "avatar_state.h"
#include "avatar_transform.h"
#include "avatar_clip_pose.h"
#include "job_lifetime.h"
#include "stream_reader_gate.h"
#include <mutex>
#include <algorithm>
#include <bit>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Compatibility patches for latent bugs in the original game code. Each one
// is narrow, documented and leaves the original body in charge.

PPC_FUNC_IMPL(__imp__sub_82437F38);

// Camera image object constructor (84 bytes from the game allocator). It
// opens a NUI image stream (82768C40) and only when that succeeds stores four
// textures at +12..+24. Its destructor (82438198) releases each nonzero slot,
// so with no Kinect the stale heap contents there are passed to the D3D
// release (824F1F50), which decrements an arbitrary word. Clear the four
// slots first, as freshly committed memory would be, then run the original.
SFR_CONCURRENT_HOOK(sub_82437F38) {
    sfr::enter_function(ctx,"sub_82437F38",0x82437F38);
    const uint32_t object=ctx.r3.u32;
    sfr::active_memory->check_write(uint64_t(object)+12,16);
    for(uint32_t offset=12;offset<28;offset+=4)
        sfr::active_memory->store<uint32_t>(uint64_t(object)+offset,0);
    __imp__sub_82437F38(ctx,base);
    std::cerr << "GAME_PATCH camera_image_slots object=0x" << std::hex << object
              << " initialized=" << std::dec << int(sfr::active_memory->load<uint8_t>(uint64_t(object)+77)) << '\n';
}

// The logical back-buffer width when it is wider than 16:9 (aspect_settings.h),
// 0 when the title keeps its own 1280 (original mode, a 16:9 or narrower window).
static uint32_t wide_logical_width() {
    using namespace sfr::aspect;
    const uint32_t width=logical_width(title_layout_width,title_layout_height);
    return wider_than_16x9(width,title_layout_height) ? width : 0;
}

PPC_FUNC_IMPL(__imp__sub_8222AB40);
PPC_FUNC_IMPL(__imp__sub_8222B058);
PPC_FUNC_IMPL(__imp__sub_82242AB8);
PPC_FUNC_IMPL(__imp__sub_823F29D8);

// 3D cameras copy their default aspect from a global (0x82B34BE0) when they
// are constructed. Refresh it from the view being drawn just before, so the
// 1280/880-wide views (menus, calibration) get 16:9 and a full-width view
// (a race) the framebuffer's, whatever the previous scene left there.
static void refresh_camera_aspect() {
    // Only while the back buffer is wider than 16:9 (aspect_settings.h);
    // camera_aspect() answers 0 otherwise as well.
    static const bool enabled=[]{ return wide_logical_width()!=0; }();
    if(!enabled || !sfr::active_guest_graphics || !sfr::active_guest_graphics->created()) return;
    const float aspect=sfr::active_guest_graphics->camera_aspect();
    if(aspect<=0.0f) return;
    const uint32_t address=0x82B34BE0;
    sfr::active_memory->store<uint32_t>(address,std::bit_cast<uint32_t>(aspect));
    sfr::active_memory->store<uint32_t>(address+4,std::bit_cast<uint32_t>(aspect*0.5f));
}
SFR_CONCURRENT_HOOK(sub_8222AB40) { sfr::enter_function(ctx,"sub_8222AB40",0x8222AB40); refresh_camera_aspect(); __imp__sub_8222AB40(ctx,base); }
SFR_CONCURRENT_HOOK(sub_8222B058) { sfr::enter_function(ctx,"sub_8222B058",0x8222B058); refresh_camera_aspect(); __imp__sub_8222B058(ctx,base); }
SFR_CONCURRENT_HOOK(sub_82242AB8) { sfr::enter_function(ctx,"sub_82242AB8",0x82242AB8); refresh_camera_aspect(); __imp__sub_82242AB8(ctx,base); }
SFR_CONCURRENT_HOOK(sub_823F29D8) { sfr::enter_function(ctx,"sub_823F29D8",0x823F29D8); refresh_camera_aspect(); __imp__sub_823F29D8(ctx,base); }

PPC_FUNC_IMPL(__imp__sub_8226CB10);
PPC_FUNC_IMPL(__imp__sub_823F18D0);

// The menus' ring and its hit test, on a wide frame.
//
// The title turns a 3D position into a point of its 1280x720 layout with two
// functions: 8226CB10 (the camera is the one in the title's services) and
// 823F18D0 (a camera given by the caller). Each takes the point through the
// camera's matrix (at +352), divides by w and maps the result onto the layout
// with 640 and 360 (the globals at 82B34BD8/82B34BDC). The ring menu places the
// selected item's quad with them and asks them where each item is when it
// tests whether the hand cursor points at it.
//
// With the wide frame's camera (its aspect is the frame's, see
// refresh_camera_aspect) the same normalized x covers k times more of the
// layout than 640 of it, which is where the GPU draws the ring: in the layout
// the item came out k times too close to its middle. The selected item was
// drawn k times too narrow, and the cursor had to be moved k times closer to
// the middle than the item looked to select it; the Back button was selected
// about 300 px left of where it is drawn. A 16:9 run of the same menu puts the
// selected item at x = 847 and 261 wide, where the wide frame gave 709 and 87.
//
// Both functions' x results are stretched about 640 by k, the camera's aspect
// over 16:9 read from its own matrix (the lengths of its x and y columns'
// first three entries). A camera that is 16:9 already reads k = 1 and is left
// alone, as is everything outside the wide mode.
//
// Where: the menus' 1280-wide view; in a race (the view is as wide as the frame
// and its HUD is placed by other means) only the pause menu's ring and widgets:
// 823F18D0 called from the ring's point-in-item test (8246B338), from the
// generic widget's textured quad (823F2318) and from its corner test (823F63F8).
static double projection_stretch(uint32_t camera,bool ring_in_race=false) {
    if(!wide_logical_width() || !sfr::active_guest_graphics) return 0.0;
    auto& memory=*sfr::active_memory;
    const bool racing=memory.load<uint32_t>(0x83E52F8C)!=0;
    if(racing ? !ring_in_race : sfr::active_guest_graphics->last_view_width()!=sfr::aspect::title_layout_width) return 0.0;
    if(!camera || !memory.readable(uint64_t(camera)+352,64)) return 0.0;
    const auto element=[&](uint32_t row,uint32_t column) {
        return double(std::bit_cast<float>(memory.load<uint32_t>(uint64_t(camera)+352+16*row+4*column)));
    };
    double x=0.0, y=0.0;
    for(uint32_t row=0;row<3;++row) { x+=element(row,0)*element(row,0); y+=element(row,1)*element(row,1); }
    x=std::sqrt(x); y=std::sqrt(y);
    if(x<1e-9) return 0.0;
    const double k=(y/x)/(double(sfr::aspect::title_layout_width)/double(sfr::aspect::title_layout_height));
    return k>1.25 && k<12.0 ? k : 0.0;
}

SFR_CONCURRENT_HOOK(sub_8226CB10) {
    sfr::enter_function(ctx,"sub_8226CB10",0x8226CB10);
    const uint32_t out=ctx.r3.u32;
    __imp__sub_8226CB10(ctx,base);
    if(!wide_logical_width()) return;
    auto& memory=*sfr::active_memory;
    const auto word=[&](uint64_t address) { return memory.readable(address,4) ? memory.load<uint32_t>(address) : 0u; };
    // The camera the function reads: [[[[83E516A0]+116]+8]+4*[[83E516A0]+116]+28]].
    const uint32_t services=word(0x83E516A0), table=services ? word(uint64_t(services)+116) : 0;
    const uint32_t cameras=table ? word(uint64_t(table)+8) : 0, index=table ? word(uint64_t(table)+28) : 0;
    const uint32_t camera=cameras ? word(uint64_t(cameras)+4ull*index) : 0;
    const double k=projection_stretch(camera);
    if(k==0.0 || !memory.readable(out,8)) return;
    const double centre=double(std::bit_cast<float>(word(0x82B34BD8)));
    if(centre<=0.0) return;
    const double value=double(std::bit_cast<float>(word(out)));
    memory.store<uint32_t>(out,std::bit_cast<uint32_t>(float(centre+(value-centre)*k)));
}

SFR_CONCURRENT_HOOK(sub_823F18D0) {
    sfr::enter_function(ctx,"sub_823F18D0",0x823F18D0);
    const uint32_t camera=ctx.r3.u32;
    // The call sites a race's pause menu uses: 8246B338's four corner tests,
    // 823F2318's four quad corners, 823F63F8's four corner tests.
    const uint32_t site=uint32_t(ctx.lr);
    const bool ring_test=site==0x8246B3A0u || site==0x8246B3E4u || site==0x8246B428u || site==0x8246B46Cu ||
                         site==0x823F2414u || site==0x823F2510u || site==0x823F2724u || site==0x823F2768u ||
                         site==0x823F6554u || site==0x823F65DCu || site==0x823F6664u || site==0x823F64CCu;
    __imp__sub_823F18D0(ctx,base);
    // The result is the x and y floats in r3's high and low words.
    if(const double k=projection_stretch(camera,ring_test); k!=0.0) {
        const uint64_t packed=ctx.r3.u64;
        const double centre=double(std::bit_cast<float>(sfr::active_memory->load<uint32_t>(0x82B34BD8)));
        if(centre>0.0) {
            const double x=double(std::bit_cast<float>(uint32_t(packed>>32)));
            const uint32_t stretched=std::bit_cast<uint32_t>(float(centre+(x-centre)*k));
            ctx.r3.u64=(uint64_t(stretched)<<32)|(packed&0xFFFFFFFFull);
        }
    }
}

PPC_FUNC_IMPL(__imp__sub_8249B068);

// Graphics object initialization. It copies the presentation parameters it is
// given (word 0 = width) into its own record at +192, and takes the 2D screen
// size (+116/+120) and the 3D aspect from there. A logical back buffer wider
// than 16:9 (aspect_settings.h: the window's shape, or SFR_ASPECT_WIDTH) widens
// the native back buffer (create_device); the guest's record has to say the
// same, or the 3D view keeps a 16:9 aspect and is stretched across the frame.
SFR_CONCURRENT_HOOK(sub_8249B068) {
    sfr::enter_function(ctx,"sub_8249B068",0x8249B068);
    if(const uint32_t value=wide_logical_width()) {
        const uint32_t parameters=ctx.r3.u32;
        if(parameters) {
            sfr::active_memory->check_write(uint64_t(parameters),4);
            if(sfr::active_memory->load<uint32_t>(uint64_t(parameters))==sfr::aspect::title_layout_width) {
                sfr::active_memory->store<uint32_t>(uint64_t(parameters),value);
                std::cerr << "GAME_PATCH aspect_width parameters=0x" << std::hex << parameters
                          << std::dec << " width=" << value << '\n';
            }
        }
        // The 3D cameras take their default aspect from a global the title keeps at
        // 16/9 (0x82B34BE0, with 360.0 before it and aspect/2 after it).
        const float aspect=float(value)/float(sfr::aspect::title_layout_height);
        const uint32_t address=0x82B34BE0;
        sfr::active_memory->check_write(address,8);
        sfr::active_memory->store<uint32_t>(address,std::bit_cast<uint32_t>(aspect));
        sfr::active_memory->store<uint32_t>(address+4,std::bit_cast<uint32_t>(aspect*0.5f));
        std::cerr << "GAME_PATCH camera_aspect=" << aspect << '\n';
    }
    __imp__sub_8249B068(ctx,base);
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
SFR_CONCURRENT_HOOK(sub_82817B48) {
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
    // Its own lock: a player's entry, once made, is its caller's alone.
    static std::mutex players_lock;
    static std::unordered_map<uint32_t,Progress> players;
    const uint32_t player=ctx.r3.u32;
    Progress& progress=[&]() -> Progress& { std::lock_guard lock(players_lock); return players[player]; }();
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

// The scene can advance from 2 to 3 after the main thread's 823B6D40 wait
// check but before worker 823B5D40 checks it. That admits a first batch whose
// actor contexts the next frame's deferred deletion would otherwise destroy
// before its next wait. Keep batches and helper queue reads alive together;
// exclusion applies only to deferred deletion, not between helpers.
namespace {
sfr::JobLifetime job_lifetime;
thread_local std::optional<sfr::JobLifetime::Lease> helper_lifetime;

void helper_leaves_job() {
    helper_lifetime.reset();
}
void lifetime_wait() {
    sfr::traced_host_wait([](void*) { job_lifetime.wait_for_change(); },
                         nullptr,"job_lifetime",1000000);
}
void acquire_lifetime(sfr::JobLifetime::Lease& lease) {
    while(!lease.try_acquire()) lifetime_wait();
}
}

// 823B5D40 is also the collision batch: at its end it pairs up the collision
// bodies that touch, sweeping each body from its owner's previous position
// along the last step's motion, both read live from the owner (823B6178).
// The Fly board (Issue #153) hands its launch direction only to the racers
// its sweep test listed. The main thread waits for the batch (823B6D40)
// after the next frame's race step (8249F048) and starts the next batch right
// after that wait, so on the console the batch, done long before, never sees
// the race step move the racers. Here it sometimes still runs when the next
// race step starts (about one race frame in 100 on an AYN Thor): it then
// sweeps the new positions, the next batch sweeps them again, and the step
// between is never swept. A racer that reached the Fly board's sphere in that
// step was never listed, boarded with the race-start direction (0,0,1) and
// flew out through the right wall. So the race step waits for the batch the
// previous frame started. SFR_STEP_WAITS_COLLISION=0 turns this off.
namespace {
bool step_waits_collision() {
    static const bool on=[]{ const char* t=std::getenv("SFR_STEP_WAITS_COLLISION"); return !t || *t!='0'; }();
    return on;
}
struct CollisionBatchGate {
    enum class State { idle, due, running };
    std::mutex mutex;
    std::condition_variable changed;
    State state=State::idle;
};
CollisionBatchGate collision_gate;
// Whether this 823B6D40 call got to its wait (the WaitForSingleObject that
// returns to 823B6DD8): frames that skip it, before the race or paused,
// start no batch.
thread_local bool collision_wait_reached=false;
void collision_batch_due() {
    std::lock_guard lock(collision_gate.mutex);
    if(collision_gate.state==CollisionBatchGate::State::idle)
        collision_gate.state=CollisionBatchGate::State::due;
}
void collision_batch_running() {
    { std::lock_guard lock(collision_gate.mutex); collision_gate.state=CollisionBatchGate::State::running; }
    collision_gate.changed.notify_all();
}
void collision_batch_finished() {
    { std::lock_guard lock(collision_gate.mutex); collision_gate.state=CollisionBatchGate::State::idle; }
    collision_gate.changed.notify_all();
}
// Before the main thread's race step: a batch the last wait let start has
// started within a millisecond here; one that has not after 8 ms is taken as
// never started. A running batch is waited for (at most 1000 ms, reported).
void wait_for_collision_batch() {
    if(!step_waits_collision() || sfr::current_guest_thread_id()!=1) return;
    using State=CollisionBatchGate::State;
    {
        std::lock_guard lock(collision_gate.mutex);
        if(collision_gate.state==State::idle) return;
    }
    struct Wait { bool finished=false, never_started=false; } wait;
    sfr::traced_host_wait([](void* argument) {
        auto& result=*static_cast<Wait*>(argument);
        std::unique_lock lock(collision_gate.mutex);
        if(!collision_gate.changed.wait_for(lock,std::chrono::milliseconds(8),
                                            []{ return collision_gate.state!=State::due; })) {
            collision_gate.state=State::idle;
            result.never_started=true;
            return;
        }
        result.finished=collision_gate.changed.wait_for(lock,std::chrono::milliseconds(1000),
                                                        []{ return collision_gate.state==State::idle; });
    },&wait,"collision_batch",1000000000);
    static std::atomic<uint32_t> waits{0},unstarted{0},timeouts{0};
    const uint32_t count=++waits;
    const uint32_t not_started=unstarted+=wait.never_started;
    const bool late=!wait.finished && !wait.never_started;
    const uint32_t late_count=timeouts+=late;
    if(count==1 || count%1024==0 || (wait.never_started && not_started<=8) || (late && late_count<=8))
        std::cerr << "GAME_PATCH collision_batch_wait count=" << count << " not_started=" << not_started
                  << " timeouts=" << late_count << '\n';
}
}

PPC_FUNC_IMPL(__imp__sub_823B6D40);
SFR_CONCURRENT_HOOK(sub_823B6D40) {
    sfr::enter_function(ctx,"sub_823B6D40",0x823B6D40);
    collision_wait_reached=false;
    __imp__sub_823B6D40(ctx,base);
    if(collision_wait_reached && step_waits_collision() && sfr::current_guest_thread_id()==1) collision_batch_due();
    collision_wait_reached=false;
}

PPC_FUNC_IMPL(__imp__sub_823B5D40);
SFR_CONCURRENT_HOOK(sub_823B5D40) {
    sfr::enter_function(ctx,"sub_823B5D40",0x823B5D40);
    sfr::JobLifetime::Lease batch(job_lifetime,sfr::JobLifetime::Kind::batch);
    acquire_lifetime(batch);
    collision_batch_running();
    struct Finished { ~Finished() { collision_batch_finished(); } } finished;
    __imp__sub_823B5D40(ctx,base);
}

PPC_FUNC_IMPL(__imp__sub_823B60C0);
SFR_CONCURRENT_HOOK(sub_823B60C0) {
    sfr::enter_function(ctx,"sub_823B60C0",0x823B60C0);
    struct Cleanup {
        ~Cleanup() { helper_leaves_job(); }
    } cleanup;
    __imp__sub_823B60C0(ctx,base);
}

// Where the generated code has the job mid-asm hooks (config/freeriders.toml),
// they do what the whole-function hooks below do by return address; the first
// one to run turns those off.
static std::atomic<bool> job_midasm_hooks{false};
static void admit_helper() {
    if(!helper_lifetime) {
        helper_lifetime.emplace(job_lifetime,sfr::JobLifetime::Kind::helper);
        acquire_lifetime(*helper_lifetime);
    }
}
static bool completion_suspend_handoff() {
    static const bool handoff=[]{
        const char* t=std::getenv("SFR_COMPLETION_SUSPEND_HANDOFF");
        return !t || *t!='0';
    }();
    return handoff;
}
namespace sfr { void prepare_worker_self_suspend(); }

// Helper 823B60C0 takes a job from the queue (82750B30 at 0x823B60F4) ...
void HelperQueueTakeMidAsmHook() { job_midasm_hooks.store(true,std::memory_order_relaxed); admit_helper(); }
// ... and leaves when the take failed (r3 nonzero) ...
void HelperQueueTakenMidAsmHook(PPCRegister& r3) { if(r3.u32!=0) helper_leaves_job(); }
// ... or when it signals the jobs it took done (SetEvent at 0x823B614C).
void HelperDoneMidAsmHook() { job_midasm_hooks.store(true,std::memory_order_relaxed); helper_leaves_job(); }
// Worker 824C39C8 signals done (SetEvent at 0x824C3A38), then suspends itself.
void WorkerDoneMidAsmHook() {
    job_midasm_hooks.store(true,std::memory_order_relaxed);
    if(completion_suspend_handoff()) sfr::prepare_worker_self_suspend();
}

PPC_FUNC_IMPL(__imp__sub_82750B30);
SFR_CONCURRENT_HOOK(sub_82750B30) {
    sfr::enter_function(ctx,"sub_82750B30",0x82750B30);
    const bool helper=ctx.lr==0x823B60F8 && !job_midasm_hooks.load(std::memory_order_relaxed);
    if(helper) admit_helper();
    __imp__sub_82750B30(ctx,base);
    // Include the interval before a successful queue take, failed takes,
    // and null callbacks. An empty poll means the last callback returned.
    if(helper && ctx.r3.u32!=0) helper_leaves_job();
}

PPC_FUNC_IMPL(__imp__sub_8249FD50);
SFR_CONCURRENT_HOOK(sub_8249FD50) {
    sfr::enter_function(ctx,"sub_8249FD50",0x8249FD50);
    if(!sfr::has_deferred_job_contexts(ctx.r3.u32,[](uint32_t address) {
        return sfr::active_memory->load<uint32_t>(address);
    })) {
        __imp__sub_8249FD50(ctx,base);
        return;
    }
    sfr::JobLifetime::Lease deletion(job_lifetime,sfr::JobLifetime::Kind::deletion);
    acquire_lifetime(deletion);
    // Keep admission closed through derived destructors and the original
    // job removal flags. A queued next batch then removes those jobs before
    // publishing work, rather than acquiring a reference to a freed actor.
    __imp__sub_8249FD50(ctx,base);
}

PPC_FUNC_IMPL(__imp__sub_824D0B18);

// SetEvent. The helper's own (return address 823B6150) says it has finished
// the jobs it took.
SFR_CONCURRENT_HOOK(sub_824D0B18) {
    sfr::enter_function(ctx,"sub_824D0B18",0x824D0B18);
    const bool by_address=!job_midasm_hooks.load(std::memory_order_relaxed);
    if(by_address && ctx.lr==0x823B6150) helper_leaves_job();
    // Worker 824C39C8 signals done and then suspends itself. A phone can run
    // the main thread's next ResumeThread between those two calls, losing
    // that resume and hanging its next completion wait (824C3A98). Register
    // the suspension before publishing done; its following self-suspend
    // consumes that registration instead of incrementing the count again.
    if(by_address && ctx.lr==0x824C3A3C && completion_suspend_handoff()) sfr::prepare_worker_self_suspend();
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
namespace {
uint32_t work_share_wait_ms() {
    static const uint32_t wait_ms=[]{
        const char* text=std::getenv("SFR_WORK_SHARE_WAIT_MS");
        const long value=text?std::strtol(text,nullptr,10):1000;
        return uint32_t(value>0?value:1000);
    }();
    return wait_ms;
}
void work_share_wait_done(uint32_t status) {
    if(status==0x102) {
        static std::atomic<uint32_t> timeouts{0};
        if(timeouts++<16) std::cerr << "GAME_PATCH work_share_wait_timeout count=" << timeouts << '\n';
    }
    // A timed-out helper wait cannot authorize reset of live jobs. Admission
    // is recorded before the helper inspects the queue, so even a helper
    // between its empty check and its callback is covered. Waits release the
    // execution permits needed by callbacks entering host hooks.
    if(!job_lifetime.helpers_idle()) {
        while(!job_lifetime.helpers_idle()) lifetime_wait();
        static std::atomic<uint32_t> waits{0};
        const uint32_t count=++waits;
        if(count<=8 || count%256==0)
            std::cerr << "GAME_PATCH job_drain_wait count=" << count << " still_running=0\n";
    }
}
}

// The same patch as mid-asm hooks (config/freeriders.toml), where the
// generated code has them: r6 before the call at 0x823B5E9C, r3 after it.
void WorkShareWaitMidAsmHook(PPCRegister& r6) {
    static std::atomic<bool> reported{false};
    if(!reported.exchange(true)) std::cerr << "GAME_PATCH work_share_wait midasm=1 ms=" << work_share_wait_ms() << char(10);
    r6.u64=work_share_wait_ms();
}
void WorkShareWaitDoneMidAsmHook(PPCRegister& r3) { work_share_wait_done(r3.u32); }

// Generated code without those hooks: the call is recognized by its return
// address and arguments (with them, r6 already holds the longer wait).
SFR_CONCURRENT_HOOK(sub_824D0B10) {
    sfr::enter_function(ctx,"sub_824D0B10",0x824D0B10);
    const bool work_share=ctx.lr==0x823B5EA0 && ctx.r3.u32==3 && ctx.r5.u32==1 && ctx.r6.u32==16;
    if(work_share) ctx.r6.u64=work_share_wait_ms();
    __imp__sub_824D0B10(ctx,base);
    if(work_share) work_share_wait_done(ctx.r3.u32);
}

PPC_FUNC_IMPL(__imp__sub_824D0150);
PPC_FUNC_IMPL(__imp__sub_8249F048);

namespace sfr { std::atomic<uint32_t> race_jobs_dispatching{0}; }

// The race step's job pool (8249F048, below): it queues the step's jobs,
// resumes its three workers 8249EF00 (console processors 0, 1 and 3) and
// waits until each has found the queue empty and signalled. The main thread
// itself takes no job, so its frame waits for the slowest worker: a phone
// that puts the workers on slow cores (Issue #176) stalls the race even with
// the main thread on its fastest core. With the workers held to an AYN
// Thor's little cores the main thread waited ~6 ms a race frame there.
// So the main thread takes queued jobs too, before its waits, with the
// worker loop's own calls (82750B30 empty test, 82750C40 take, the job's
// second virtual function), as processor 0 - the processor its own worker
// 8249EF00 has, so a job sees no processor it could not have seen before.
// The workers take jobs beside it; once the queue is empty they signal and
// suspend, and the waits stay as they were. (With SFR_PARALLEL_WORKER=cores
// the processor-0 worker then runs only after the main thread has given its
// processor back at the wait, and finds the queue empty.)
// SFR_MAIN_HELPS_JOBS=0 (a Test option) turns this off. In that little-core
// case a Dolphin Resort race ran at 0.78-0.85 of real time instead of 0.61; in a
// normal Thor race the main thread spent ~0.75 ms a race frame in the
// dispatch instead of ~0.95.
namespace {
bool main_helps_jobs() {
    static const bool on=[]{ const char* t=std::getenv("SFR_MAIN_HELPS_JOBS"); return !t || *t!='0'; }();
    return on;
}
constexpr uint32_t race_job_queue_pointer=0x83E52CD0;
constexpr uint32_t race_job_worker_take_return=0x8249EF40;  // worker 8249EF00's take
thread_local bool race_jobs_waiting=false;  // in 8249F048, before its first wait
// RACE_JOBS (every 600 dispatches): who ran the race step's jobs and how long
// the main thread spent running them and waiting for the workers.
struct RaceJobCounters {
    std::atomic<uint64_t> dispatches{0}, main_jobs{0}, worker_jobs{0}, help_ns{0}, wait_ns{0}, collision_ns{0};
};
RaceJobCounters race_job_counters;
uint64_t steady_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
}
static void help_race_jobs(PPCContext& __restrict ctx, uint8_t* base) {
    const uint64_t started=steady_ns();
    const PPCRegister r1=ctx.r1, r3=ctx.r3, r4=ctx.r4;
    const uint64_t lr=ctx.lr;
    // A frame of the worker's size below the caller's, with its back chain;
    // the take writes the job at +80, as in the worker.
    ctx.r1.u64=r1.u32-128;
    sfr::active_memory->store<uint32_t>(ctx.r1.u32,r1.u32);
    const uint32_t slot=ctx.r1.u32+80;
    uint64_t jobs=0;
    for(;;) {
        const uint32_t queue=sfr::active_memory->load<uint32_t>(race_job_queue_pointer);
        if(!queue) break;
        ctx.r3.u64=queue;
        sub_82750B30(ctx,base);
        if(ctx.r3.u32!=0) break;  // empty
        sfr::active_memory->store<uint32_t>(slot,0);
        ctx.r3.u64=queue;
        ctx.r4.u64=slot;
        sub_82750C40(ctx,base);
        if(ctx.r3.s32<0) continue;  // lost a race for the entry: try again
        const uint32_t job=sfr::active_memory->load<uint32_t>(slot);
        if(!job) continue;
        const uint32_t call=sfr::active_memory->load<uint32_t>(
            uint64_t(sfr::active_memory->load<uint32_t>(job))+4);
        ctx.r3.u64=job;
        ctx.ctr.u64=call;
        PPC_CALL_INDIRECT_FUNC(call);
        ++jobs;
    }
    ctx.r1=r1, ctx.r3=r3, ctx.r4=r4, ctx.lr=lr;
    race_job_counters.main_jobs.fetch_add(jobs,std::memory_order_relaxed);
    race_job_counters.help_ns.fetch_add(steady_ns()-started,std::memory_order_relaxed);
}

// While 8249F048 runs, its jobs are expected to run (SFR_WATCH_RACERS counts
// the racer readers that run outside it).
SFR_CONCURRENT_HOOK(sub_8249F048) {
    sfr::enter_function(ctx,"sub_8249F048",0x8249F048);
    const uint64_t collision_started=steady_ns();
    wait_for_collision_batch();
    race_job_counters.collision_ns.fetch_add(steady_ns()-collision_started,std::memory_order_relaxed);
    sfr::race_jobs_dispatching.fetch_add(1,std::memory_order_relaxed);
    struct Done { ~Done() { sfr::race_jobs_dispatching.fetch_sub(1,std::memory_order_relaxed); race_jobs_waiting=false; } } done;
    race_jobs_waiting=sfr::current_guest_thread_id()==1;
    __imp__sub_8249F048(ctx,base);
}

// The second job pool has the same flaw (Issue #64). Its dispatcher 8249F048
// queues a race step's jobs, resumes the pool's three workers (8249EF00),
// then waits for each worker's done event with WaitForSingleObject(event,
// 16 ms) and ignores the result. A worker still in a job after 16 ms - more
// likely on a 4-core PC - kept walking that job's racer list while the game
// went on to reset it, and read through an emptied slot (sub_82327CF8 and
// sub_82326518, the reads at +156). A worker signals its event only once the
// queue is empty and its own job has returned, so the waits get
// SFR_RACE_JOB_WAIT_MS (default 1000; 16 is the original) instead.
SFR_CONCURRENT_HOOK(sub_824D0150) {
    sfr::enter_function(ctx,"sub_824D0150",0x824D0150);
    static const uint32_t wait_ms=[]{
        const char* text=std::getenv("SFR_RACE_JOB_WAIT_MS");
        const long value=text?std::strtol(text,nullptr,10):1000;
        return uint32_t(value>0?value:1000);
    }();
    if(ctx.lr==0x823B6DD8) collision_wait_reached=true;  // 823B6D40's wait for the collision batch
    const bool race_jobs=ctx.lr==0x8249F114 && ctx.r4.u32==16;
    if(race_jobs) {
        static std::atomic<bool> reported{false};
        if(!reported.exchange(true)) std::cerr << "GAME_PATCH race_job_wait ms=" << wait_ms << '\n';
        ctx.r4.u64=wait_ms;
    }
    // The first of the main thread's three waits in this dispatch.
    const bool first_main_wait=race_jobs && race_jobs_waiting;
    if(first_main_wait) {
        race_jobs_waiting=false;
        if(main_helps_jobs()) help_race_jobs(ctx,base);
        const uint64_t dispatches=race_job_counters.dispatches.fetch_add(1,std::memory_order_relaxed)+1;
        if(dispatches%600==0) {
            const double n=600.0;
            std::cerr << "RACE_JOBS dispatches=" << dispatches << " main_helps=" << main_helps_jobs()
                      << " jobs_main=" << race_job_counters.main_jobs.exchange(0,std::memory_order_relaxed)/n
                      << " jobs_workers=" << race_job_counters.worker_jobs.exchange(0,std::memory_order_relaxed)/n
                      << " help_ms=" << race_job_counters.help_ns.exchange(0,std::memory_order_relaxed)/n/1e6
                      << " wait_ms=" << race_job_counters.wait_ns.exchange(0,std::memory_order_relaxed)/n/1e6
                      << " collision_wait_ms=" << race_job_counters.collision_ns.exchange(0,std::memory_order_relaxed)/n/1e6
                      << '\n';
        }
    }
    const bool collision=ctx.lr==0x823B6DD8;
    const uint64_t wait_started=(race_jobs || collision) && sfr::current_guest_thread_id()==1 ? steady_ns() : 0;
    __imp__sub_824D0150(ctx,base);
    if(wait_started) (race_jobs ? race_job_counters.wait_ns : race_job_counters.collision_ns)
        .fetch_add(steady_ns()-wait_started,std::memory_order_relaxed);
    if(race_jobs && ctx.r3.u32==0x102) {
        static std::atomic<uint32_t> timeouts{0};
        const uint32_t count=++timeouts;
        if(count<=16 || count%256==0) std::cerr << "GAME_PATCH race_job_wait_timeout count=" << count << '\n';
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
SFR_CONCURRENT_HOOK(sub_82A53BC0) {
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
    if(caller==race_job_worker_take_return && ctx.r3.s32>=0 && slot && sfr::active_memory->load<uint32_t>(slot))
        race_job_counters.worker_jobs.fetch_add(1,std::memory_order_relaxed);
    if(!sfr::active_memory || !slot || (caller!=0x823B5E58 && caller!=0x823B6114)) return;
    uint32_t job=0;
    try { job=sfr::active_memory->load<uint32_t>(slot); } catch(...) { return; }
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

// The file stream's reader must be waiting before its dispatcher hands it a
// read (stream_reader_gate.h): a read handed over earlier is cleared by the
// reader's first loop, which then dereferences the null request (the Mac's
// startup GUEST_ACCESS_VIOLATION guest=0x10 in 8249C100). The handoff
// 8249D368 waits for the reader to reach its wait, without its execution
// permit, for at most SFR_STREAM_READER_WAIT_MS (default 5000; 0 turns the
// wait off and leaves only the reports).
namespace {
sfr::StreamReaderGate stream_reader_gate;
uint32_t stream_reader_wait_ms() {
    static const uint32_t wait_ms=[]{
        const char* text=std::getenv("SFR_STREAM_READER_WAIT_MS");
        const long value=text?std::strtol(text,nullptr,10):5000;
        return uint32_t(value>0?value:0);
    }();
    return wait_ms;
}
}

PPC_FUNC_IMPL(__imp__sub_8249C100);
// The reader's thread function. SFR_TEST_STREAM_READER_DELAY_MS (a test
// switch, off by default) holds it back that long before it runs, as a busy
// host can: with SFR_STREAM_READER_WAIT_MS=0 the first read is then lost.
SFR_CONCURRENT_HOOK(sub_8249C100) {
    sfr::enter_function(ctx,"sub_8249C100",0x8249C100);
    static const uint32_t delay_ms=[]{
        const char* text=std::getenv("SFR_TEST_STREAM_READER_DELAY_MS");
        const long value=text?std::strtol(text,nullptr,10):0;
        return uint32_t(value>0?value:0);
    }();
    if(delay_ms) {
        std::cerr << "GAME_PATCH stream_reader_test_delay ms=" << delay_ms << '\n';
        uint32_t ms=delay_ms;
        sfr::traced_host_wait([](void* argument) {
            std::this_thread::sleep_for(std::chrono::milliseconds(*static_cast<uint32_t*>(argument)));
        },&ms,"stream_reader_test_delay",int64_t(delay_ms)*1000000);
    }
    __imp__sub_8249C100(ctx,base);
}

PPC_FUNC_IMPL(__imp__sub_824B2060);
// A thread object's wait for its own event (with no thread object, a Sleep).
SFR_CONCURRENT_HOOK(sub_824B2060) {
    using namespace sfr::stream_reader;
    const bool reader=ctx.lr==idle_wait_return;
    // The reader (8249C100): r19 is the manager, r30 its own thread object,
    // and it has just cleared the request. Said before the entry, which can
    // hand the execution permit over and keep the dispatcher waiting.
    const uint32_t manager=ctx.r19.u32, thread=ctx.r30.u32;
    if(reader) stream_reader_gate.reader_idle(thread);
    sfr::enter_function(ctx,"sub_824B2060",0x824B2060);
    __imp__sub_824B2060(ctx,base);
    if(!reader) return;
    // Woken with no request and not told to finish: the lost handoff, which
    // the reader's next load (+16 of the null request) turns into a fault.
    if(sfr::active_memory->load<uint32_t>(uint64_t(manager)+manager_request)==0 &&
       sfr::active_memory->load<uint8_t>(uint64_t(thread)+thread_finished)==0) {
        std::ostringstream text;
        text << "GAME_PATCH stream_reader_empty_wake manager=0x" << std::hex << manager << " thread=0x" << thread << '\n';
        // Flushed: the fault that follows would lose the buffered trace.
        std::cerr << text.str() << std::flush;
        std::fflush(stderr);
    }
}

PPC_FUNC_IMPL(__imp__sub_8249D368);
// The dispatcher's handoff of one read to the reader (r3 the manager).
SFR_CONCURRENT_HOOK(sub_8249D368) {
    sfr::enter_function(ctx,"sub_8249D368",0x8249D368);
    using namespace sfr::stream_reader;
    const uint32_t manager=ctx.r3.u32;
    auto reader=[manager]{
        return sfr::active_memory->load<uint32_t>(uint64_t(manager)+manager_reader_thread);
    };
    if(!stream_reader_gate.ready(reader())) {
        const auto started=std::chrono::steady_clock::now();
        bool ready=false;
        if(const uint32_t limit=stream_reader_wait_ms()) {
            struct Wait { uint32_t manager, limit; bool ready; } wait{manager,limit,false};
            sfr::traced_host_wait([](void* argument) {
                auto& wait=*static_cast<Wait*>(argument);
                wait.ready=stream_reader_gate.wait_until_ready([&wait]{
                    return sfr::active_memory->load<uint32_t>(uint64_t(wait.manager)+manager_reader_thread);
                },std::chrono::milliseconds(wait.limit));
            },&wait,"stream_reader",int64_t(limit)*1000000);
            ready=wait.ready;
        }
        static std::atomic<uint32_t> reports{0};
        if(reports++<16) {
            // One write, so that other threads' lines do not split it.
            const uint32_t thread=reader();
            std::ostringstream text;
            text << "GAME_PATCH stream_reader_wait manager=0x" << std::hex << manager << " thread=0x" << thread
                 << std::dec << " resumed="
                 << (thread ? int(sfr::active_memory->load<uint8_t>(uint64_t(thread)+thread_resumed)) : -1)
                 << " ready=" << ready << " ms="
                 << std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count()
                 << " wait_ms=" << stream_reader_wait_ms() << '\n';
            std::cerr << text.str() << std::flush;
            std::fflush(stderr);
        }
    }
    __imp__sub_8249D368(ctx,base);
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
// The walkers read [list] as the count and [list+4] as the parts, 96 bytes
// each (823BA468; 823B9F00's are 52). Online, a PC loading a race stopped in
// 823BA468 with a count but parts that pointed nowhere, so the parts must be
// readable too; a count past anything an avatar has, parts that cannot be
// read, or a chain that cannot be read is no body to draw, and is skipped
// like an empty avatar rather than followed.
bool avatar_has_parts(uint32_t avatar) {
    if(!sfr::active_memory || !avatar) return true;
    auto& memory=*sfr::active_memory;
    const auto refuse=[](const char* why) {
        static std::atomic<uint32_t> reported{0};
        if(reported.fetch_add(1,std::memory_order_relaxed)<8)
            std::cerr << "GAME_PATCH avatar_parts_unusable reason=" << why << char(10);
        return false;
    };
    try {
        const uint32_t list=memory.load<uint32_t>(uint64_t(avatar)+0x2FC40);
        if(!list) return false;
        const uint32_t entry=memory.load<uint32_t>(list);
        if(!entry) return false;
        const uint32_t count=memory.load<uint32_t>(entry);
        if(!count) return false;
        if(count>256) return refuse("count");
        const uint32_t parts=memory.load<uint32_t>(uint64_t(entry)+4);
        if(!parts || !memory.readable(parts,uint64_t(count)*96)) return refuse("parts");
        return true;
    } catch(...) { return refuse("chain"); }
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
    SFR_CONCURRENT_HOOK(sub_##address) { \
        sfr::enter_function(ctx,"sub_" #address,0x##address); \
        if(!avatar_has_parts(ctx.r3.u32)) { note_avatar_skip(0x##address); return; } \
        __imp__sub_##address(ctx,base); \
    }

PPC_FUNC_IMPL(__imp__sub_823B9D60);
SFR_CONCURRENT_HOOK(sub_823B9D60) {
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
SFR_CONCURRENT_HOOK(sub_823B9F00) {
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
// Written as a clip is evaluated, read as the hands and the model are drawn,
// perhaps on other threads: under avatar_clips_lock, held for the copy only.
std::array<sfr::AvatarClipPose, 2> avatar_clips;
std::mutex avatar_clips_lock;
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
SFR_CONCURRENT_HOOK(sub_823BBCB8) {
    sfr::enter_function(ctx, "sub_823BBCB8", 0x823BBCB8);
    const uint32_t controller = ctx.r3.u32, renderer = ctx.r4.u32, animation = ctx.r5.u32;
    __imp__sub_823BBCB8(ctx, base);
    if (!sfr::active_memory) return;
    const auto& memory = *sfr::active_memory;
    const auto owner = sfr::local_avatar_for_animation(memory, controller, renderer, animation);
    if (!owner) return;
    // Preserve evaluated/blended clip output before tracked shoulder corrections.
    const sfr::AvatarClipPose clip{owner->rider, animation, sfr::present_count.load(), read_avatar_pose(memory, animation), owner->manager, renderer, controller};
    std::lock_guard lock(avatar_clips_lock);
    avatar_clips[owner->local_slot] = clip;
}
PPC_FUNC_IMPL(__imp__sub_823BC330);
SFR_CONCURRENT_HOOK(sub_823BC330) {
    sfr::enter_function(ctx, "sub_823BC330", 0x823BC330);
    // Only the hand attachment getter used by 8227FD58. Gameplay IK and
    // all other native palette consumers retain the original SDK matrices.
    if (ctx.lr == 0x8227FDA4 && (ctx.r4.u32 == 33 || ctx.r4.u32 == 36) && sfr::active_memory) {
        auto& memory = *sfr::active_memory;
        const auto owner = sfr::local_avatar_for_animation(memory, ctx.r3.u32);
        if (owner && memory.readable(ctx.r5.u32, 64)) {
            auto pose = [&] {
                std::lock_guard lock(avatar_clips_lock);
                return avatar_clips[owner->local_slot].current(owner->rider, owner->animation, sfr::present_count.load(), owner->manager, owner->renderer, owner->controller);
            }();
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
SFR_CONCURRENT_HOOK(sub_823B97A8) {
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
                {
                    std::lock_guard lock(avatar_clips_lock);
                    if (auto clip = avatar_clips[owner->local_slot].current(rider, ctx.r4.u32, sfr::present_count.load(), owner->manager, owner->renderer, owner->controller)) frame.pose = *clip;
                }
                // BA028 reflects the completed skeleton across X for goofy.
                if (memory.readable(uint64_t(ctx.r3.u32) + 8, 1))
                    frame.pose.mirrored = memory.load<uint8_t>(uint64_t(ctx.r3.u32) + 8) != 0;
                frame.present = sfr::present_count.load();
                {
                    sfr::HostGraphicsScope graphics;
                    sfr::draw_avatar_model(frame);
                }
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

// SFR_AUDIO_CUES=1: every cue the CRI sound library looks up, by name
// (827BCD18), by name in a given sheet (827BCE00) or by ID (827BCFA8), with
// the caller and the present it was asked on. A voice line that does not fit
// the race (a winning line after a loss) shows here as the cue the game asked
// for, which tells the game's choice apart from the wrong sound being played.
static bool audio_cue_trace() {
    static const bool on=[]{ const char* t=std::getenv("SFR_AUDIO_CUES"); return t && *t && *t!='0'; }();
    return on;
}
static std::string guest_cue_name(uint32_t address) {
    std::string name;
    if(address<0x40000000u) return name;
    try {
        for(uint32_t i=0;i<64;++i) {
            const char c=char(sfr::active_memory->load<uint8_t>(uint64_t(address)+i));
            if(!c) break;
            if(c<0x20 || c>0x7E) return {};
            name+=c;
        }
    } catch(...) { return {}; }
    return name;
}
static void report_cue(const char* how, PPCContext& ctx, uint32_t key, bool named) {
    std::ostringstream line;
    line << "AUDIO_CUE " << how << " present=" << sfr::present_count.load(std::memory_order_relaxed);
    if(named) line << " name=" << guest_cue_name(key);
    line << " key=0x" << std::hex << key << " r3=0x" << ctx.r3.u32 << " r5=0x" << ctx.r5.u32
         << " lr=0x" << uint32_t(ctx.lr);
    std::cerr << line.str() << '\n';
}

PPC_FUNC_IMPL(__imp__sub_827BCD18);
SFR_CONCURRENT_HOOK(sub_827BCD18) {
    sfr::enter_function(ctx,"sub_827BCD18",0x827BCD18);
    if(audio_cue_trace()) report_cue("by_name",ctx,ctx.r4.u32,true);
    __imp__sub_827BCD18(ctx,base);
}
PPC_FUNC_IMPL(__imp__sub_827BCE00);
SFR_CONCURRENT_HOOK(sub_827BCE00) {
    sfr::enter_function(ctx,"sub_827BCE00",0x827BCE00);
    if(audio_cue_trace()) report_cue("in_sheet",ctx,ctx.r4.u32,true);
    __imp__sub_827BCE00(ctx,base);
}
PPC_FUNC_IMPL(__imp__sub_827BCFA8);
SFR_CONCURRENT_HOOK(sub_827BCFA8) {
    sfr::enter_function(ctx,"sub_827BCFA8",0x827BCFA8);
    if(audio_cue_trace()) report_cue("by_id",ctx,ctx.r4.u32,false);
    __imp__sub_827BCFA8(ctx,base);
}

// SFR_DUMP_DECOMPRESSED=<directory>: everything the game's LZX decoder
// produces, for making mods of the compressed files. The decoder is a stream:
// 824DE658 starts one (and returns its context), then each call of
// 824DE650 (context, destination, &destination size, source, source size)
// returns the next part of the file. Each part goes to <directory>/<n>.bin,
// with a line in index.txt: "<n> <size> <source size> <context> <first 64
// source bytes>", which find the file on the disc; a stream's start is
// "R <context>". scripts/unpack_assets.py joins the parts. Off unless set.
static const std::string& decompressed_dump() {
    static const std::string directory=[]{ const char* t=std::getenv("SFR_DUMP_DECOMPRESSED"); return std::string(t?t:""); }();
    return directory;
}
static std::mutex decompressed_dump_lock;

PPC_FUNC_IMPL(__imp__sub_824DE658);
SFR_CONCURRENT_HOOK(sub_824DE658) {
    sfr::enter_function(ctx,"sub_824DE658",0x824DE658);
    __imp__sub_824DE658(ctx,base);
    if(decompressed_dump().empty()) return;
    std::lock_guard guard(decompressed_dump_lock);
    std::ofstream(decompressed_dump()+"/index.txt",std::ios::app) << "R " << std::hex << ctx.r3.u32 << '\n';
}

PPC_FUNC_IMPL(__imp__sub_824DE650);
SFR_CONCURRENT_HOOK(sub_824DE650) {
    sfr::enter_function(ctx,"sub_824DE650",0x824DE650);
    const std::string& directory=decompressed_dump();
    if(directory.empty()) { __imp__sub_824DE650(ctx,base); return; }
    const uint32_t context=ctx.r3.u32, destination=ctx.r4.u32, size_at=ctx.r5.u32, source=ctx.r6.u32,
                   source_size=ctx.r7.u32;
    __imp__sub_824DE650(ctx,base);
    if(ctx.r3.s32<0) return;
    auto& memory=*sfr::active_memory;
    const uint32_t produced=memory.load<uint32_t>(size_at);
    static uint32_t count=0;
    std::lock_guard guard(decompressed_dump_lock);
    const uint32_t n=count++;
    std::vector<char> bytes(produced);
    for(uint32_t i=0;i<produced;++i) bytes[i]=char(memory.load<uint8_t>(uint64_t(destination)+i));
    std::ofstream(directory+"/"+std::to_string(n)+".bin",std::ios::binary).write(bytes.data(),std::streamsize(bytes.size()));
    std::ostringstream line;
    line<<n<<' '<<produced<<' '<<source_size<<' '<<std::hex<<context<<' ';
    for(uint32_t i=0;i<64 && i<source_size;++i) {
        const uint32_t b=memory.load<uint8_t>(uint64_t(source)+i);
        line<<(b<16?"0":"")<<b;
    }
    std::ofstream(directory+"/index.txt",std::ios::app)<<line.str()<<'\n';
}
