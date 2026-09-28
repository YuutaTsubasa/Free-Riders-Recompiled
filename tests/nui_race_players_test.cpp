#include "ppc_recomp_shared.h"
#include "diagnostic_hooks.h"
#include "nui_race.h"
#include <bit>
#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace harness {
using Hook = void (*)(PPCContext&, uint8_t*);
auto& hooks() { static std::unordered_map<std::string, Hook> value; return value; }
bool register_hook(const char* name, Hook hook) { hooks().emplace(name, hook); return true; }
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
sfr::GamepadState first;
std::optional<sfr::GamepadState> second;
constexpr uint32_t box = 0x10000000, original = 0x10001000, other = 0x10003000;
constexpr uint32_t next_other = 0x10005000, sources = 0x10008000, objects = 0x10008100;
constexpr uint32_t vtable = 0x10008200, results = 0x10008300, entries = 0x10008400;
constexpr uint32_t detector = 0x10009000, selected = entries + 84;
constexpr uint32_t nui_box_global = 0x83E52F88, race_flag_global = 0x83E52F8C;
constexpr uint32_t injected = 0x71730000, reader = 0x82918418;
uint32_t manager_rebind_object = 0, manager_rebind_record = 0;
unsigned original_calls = 0;
uint32_t original_overthrow_result = 99;
uint32_t original_kick_bits = 0;
bool consumer_throws = false;
bool camera_active = false;
uint64_t camera_sequence=0;
uint64_t camera_now=1000000000;
uint32_t consumed_record = 0;
}

// Capture production registration, including the same entry points used by
// generated guest code. Only guest/input boundaries are supplied below.
#undef SFR_HOOK
#define SFR_HOOK(name) PPC_FUNC(name); \
    static const bool name##_registered = harness::register_hook(#name, name); PPC_FUNC(name)
#include "../src/nui_race_hooks.cpp"

namespace sfr {
GuestMemory* active_memory = nullptr;
bool camera_motion_active() { return harness::camera_active; }
uint64_t camera_pose_generation() { return harness::camera_sequence; }
uint64_t camera_motion_clock_ns() { return harness::camera_now; }
GamepadState nui_gamepad() { return harness::first; }
bool nui_body_from_sensor() { return false; }
uint64_t kinect_frame_generation() { return 0; }
std::optional<GamepadState> second_player_pad() { return harness::second; }
void enter_function_observed(PPCContext&, const char*, uint32_t) {}
void guest_checkpoint_permit() {}
void call_indirect(PPCContext& ctx, uint8_t*, uint32_t method) {
    harness::require(method == harness::reader, "unexpected body reader");
    // Exact guest 82918418 boundary: lwz r3,4(r3); blr.
    ctx.r3.u64 = active_memory->load<uint32_t>(ctx.r3.u32 + 4);
}
}
PPC_FUNC(__imp__sub_82438930) {
    if (harness::manager_rebind_object) {
        sfr::active_memory->store<uint32_t>(harness::manager_rebind_object + 4, harness::manager_rebind_record);
        harness::manager_rebind_object = 0;
    }
}
PPC_FUNC(__imp__sub_82918418) {
    ctx.r3.u64 = sfr::active_memory->load<uint32_t>(ctx.r3.u32 + 4);
}
PPC_FUNC(__imp__sub_822C6200) {
    if (harness::consumer_throws) throw std::runtime_error("fixture consumer stopped");
    // The original 822C6200 updates the object, then calls its body reader
    // before reading lean. Model that boundary without copying its algorithm.
    auto& m = *sfr::active_memory;
    const uint32_t object = m.load<uint32_t>(ctx.r3.u32);
    const uint32_t record = m.load<uint32_t>(object + 4);
    if (record) m.store<uint32_t>(record + 640, 0);
    ctx.r3.u64 = object;
    auto found = harness::hooks().find("sub_82918418");
    if (found != harness::hooks().end()) found->second(ctx, base);
    else __imp__sub_82918418(ctx, base);
    harness::consumed_record = ctx.r3.u32;
}
#define ORIGINAL(address) PPC_FUNC(__imp__sub_##address) { ++harness::original_calls; ctx.r3.u64 = 99; }
ORIGINAL(822C9050)
ORIGINAL(822B60F8)
ORIGINAL(822C8778)
ORIGINAL(822CB840)
ORIGINAL(822C8650)
ORIGINAL(822C9180)
ORIGINAL(822C9A80)
ORIGINAL(822CAF48)
ORIGINAL(822CA6B0)
ORIGINAL(822C9BF0)
ORIGINAL(822CB0B8)
PPC_FUNC(__imp__sub_822CA518) {
    ++harness::original_calls;
    auto& m=*sfr::active_memory;
    const uint32_t e=m.load<uint32_t>(ctx.r5.u32)+84*m.load<uint32_t>(ctx.r5.u32+8);
    m.store<uint32_t>(e+4,m.load<uint32_t>(e+4)|harness::original_kick_bits);
    ctx.r3.u64=harness::original_kick_bits==0x1000000?1:harness::original_kick_bits?0:99;
}
PPC_FUNC(__imp__sub_822CBD28) { __imp__sub_822CA518(ctx,base); }
ORIGINAL(822CA810)
ORIGINAL(822CBF30)
ORIGINAL(822CAC90)
ORIGINAL(822C9938)
ORIGINAL(822CC2D0)
PPC_FUNC(__imp__sub_822CC590) { ++harness::original_calls; ctx.r3.u64=harness::original_overthrow_result; }
ORIGINAL(822CCD98)
ORIGINAL(822CD068)
ORIGINAL(822CDDB0)
ORIGINAL(822CC8F0)
ORIGINAL(822CDEE0)
ORIGINAL(822CD638)
ORIGINAL(822CD960)
ORIGINAL(822CD3F0)
ORIGINAL(822CE118)
ORIGINAL(822CB9B8)
ORIGINAL(822B72E0)
PPC_FUNC(sub_822C8958) { throw std::runtime_error("unexpected grouped detector"); }

namespace harness {
auto& mem() { return *sfr::active_memory; }
float f(uint32_t address) { return std::bit_cast<float>(mem().load<uint32_t>(address)); }
uint32_t invoke(const char* name, unsigned source = 0, unsigned instance = 0) {
    PPCContext ctx;
    ctx.r3.u64 = detector + instance * 128;
    ctx.r4.u64 = sources + source * 4;
    ctx.r5.u64 = results;
    auto found = hooks().find(name);
    if (found != hooks().end()) found->second(ctx, mem().base());
    else if (std::string(name) == "sub_822CA6B0") __imp__sub_822CA6B0(ctx, mem().base());
    else throw std::runtime_error(std::string("unregistered hook ") + name);
    return ctx.r3.u32;
}
void bind(unsigned source, unsigned object, uint32_t record) {
    mem().store<uint32_t>(sources + source * 4, objects + object * 16);
    mem().store<uint32_t>(objects + object * 16, vtable);
    mem().store<uint32_t>(objects + object * 16 + 4, record);
}
void frame() { ++camera_sequence;camera_now+=16666667;invoke("sub_82438930"); }
void consume(unsigned source) {
    PPCContext ctx;
    ctx.r3.u64 = sources + source * 4;
    auto found = hooks().find("sub_822C6200");
    if (found != hooks().end()) found->second(ctx, mem().base());
    else __imp__sub_822C6200(ctx, mem().base());
}
void reset_results() {
    for (unsigned off = 0; off < 84; off += 4) mem().store<uint32_t>(selected + off, 0);
    mem().store<uint32_t>(selected + 4, 0x20);
    mem().store<uint32_t>(selected + 20, 0x8);
}
bool braking(unsigned source) {
    reset_results();
    return invoke("sub_822C9BF0", source) == 1;
}
void begin() {
    original_kick_bits=0;
    mem().store<uint32_t>(0x83E516A0,0);
    camera_active = false;
    mem().store<uint32_t>(race_flag_global, 0);
    frame();
    first = {};
    second = sfr::GamepadState{};
    first.buttons = sfr::gamepad_button::b;
    first.thumb_ly = -32768;
    mem().store<uint32_t>(box + 0x78, original);
    mem().store<uint32_t>(race_flag_global, 1);
    frame();
    bind(0, 0, injected);
    bind(1, 1, other);
    bind(2, 2, injected);
    bind(3, 3, other);
}
void baseline() {
    require(braking(0) && braking(2), "both P1 objects must use the first pad");
    require(!braking(1) && !braking(3), "both P2 objects must use the second pad");
    first = {};
    second->buttons = sfr::gamepad_button::b;
    second->thumb_ly = -32768;
    frame();
    require(!braking(0) && braking(1), "braking must remain independent");
    require(mem().load<uint8_t>(selected + 80) == 100, "P2 brake strength must reach its selected result");
    require(mem().load<uint32_t>(selected + 4) == (0x20 | 0x400100), "brake must preserve existing result bits");
    require(mem().load<uint32_t>(entries + 4) == 0, "other result entries must remain untouched");
}
void source_reuse() {
    require(braking(0), "source must begin as P1");
    bind(0, 4, other);
    require(!braking(0), "reused source must follow its new P2 object");
}
void record_rebind() {
    require(braking(0), "object must begin as P1");
    bind(0, 0, other);
    require(!braking(0), "same object must follow its changed P2 body");
}
void zero_then_valid() {
    bind(4, 4, 0);
    braking(4);
    bind(4, 4, other);
    require(!braking(4), "initial zero must not permanently classify P2 as P1");
}
void original_alias() {
    bind(4, 4, original);
    require(braking(4), "the original manager body remains a P1 alias");
    frame();
    require(mem().load<uint32_t>(original + 268) == 1, "P1 alias body fields must agree with P1 detectors");
}
void disconnect() {
    require(!braking(1), "P2 starts neutral");
    second.reset();
    frame();
    require(!braking(1), "disconnected P2 must never inherit P1 braking");
    require(mem().load<uint32_t>(other + 268) == 0, "disconnected P2 body must be neutral");
    second = sfr::GamepadState{};
    second->buttons = sfr::gamepad_button::b;
    first = {};
    frame();
    require(braking(1) && !braking(0), "reconnected P2 must retain ownership");
}
void disconnect_held_actions() {
    second->buttons = sfr::gamepad_button::a | sfr::gamepad_button::x;
    frame();
    reset_results(); invoke("sub_822C9050", 1);
    second.reset(); frame();
    reset_results();
    require(invoke("sub_822C9050", 1) == 2, "disconnecting held A must not jump P2");
    require(invoke("sub_822CA518", 1) == 2, "disconnecting held X must not kick P2");
}
void body_rebind() {
    braking(1);
    second->buttons = sfr::gamepad_button::b;
    mem().store<uint32_t>(other + 268, 0x1357);
    manager_rebind_object = objects + 16;
    manager_rebind_record = next_other;
    frame();
    consume(1);
    require(mem().load<uint32_t>(next_other + 268) == 1, "live consumer must apply pad fields to the current P2 record");
    require(mem().load<uint32_t>(other + 268) == 0x1357, "manager update must not write the stale P2 record");
}
void manager_replacement() {
    braking(1);
    mem().store<uint32_t>(other + 268, 0x2468);
    constexpr uint32_t new_box = 0x1000A000, new_original = 0x1000B000;
    mem().store<uint32_t>(new_box + 0x78, new_original);
    mem().store<uint32_t>(nui_box_global, new_box);
    frame();
    const bool retired_untouched = mem().load<uint32_t>(other + 268) == 0x2468;
    bind(0, 0, new_original);
    const bool alias_is_primary = braking(0);
    mem().store<uint32_t>(race_flag_global, 0); frame();
    mem().store<uint32_t>(nui_box_global, box);
    require(retired_untouched, "manager replacement must retire the preceding race's second record");
    require(alias_is_primary, "replacement manager original must remain P1");
}
void manager_record_replacement() {
    braking(1);
    mem().store<uint32_t>(other + 268, 0x3579);
    mem().store<uint32_t>(box + 0x78, next_other);
    frame();
    require(mem().load<uint32_t>(other + 268) == 0x3579, "replacing manager body must retire preceding second records");
    bind(0, 0, next_other);
    require(braking(0), "replacement original body in same manager must be P1");
}
void retired_source() {
    braking(1);
    // A freed source's mapped heap bytes are now unrelated data. The title
    // never supplies this source again, so a manager tick must not read it.
    mem().store<uint32_t>(sources + 4, 0xDEAD0000);
    mem().store<uint32_t>(other + 268, 0x468A);
    bool survived = true;
    try { frame(); } catch (const sfr::RuntimeStop&) { survived = false; }
    bind(1, 1, other);
    require(survived, "manager must not dereference a retired source/object");
    require(mem().load<uint32_t>(other + 268) == 0x468A, "retired source must not authorize a later body write");
}
void replacement_after_disconnect() {
    braking(1);
    second.reset(); frame();
    constexpr uint32_t new_box = 0x1000A000, new_original = 0x1000B000;
    mem().store<uint32_t>(new_box + 0x78, new_original);
    mem().store<uint32_t>(nui_box_global, new_box);
    frame();
    bind(1, 1, other);
    const bool inherited_first = braking(1);
    mem().store<uint32_t>(race_flag_global, 0); frame();
    mem().store<uint32_t>(nui_box_global, box);
    require(!inherited_first, "manager replacement during a race must preserve disconnected P2 ownership");
}
void missing_manager_after_disconnect() {
    braking(1);
    second.reset();
    mem().store<uint32_t>(nui_box_global, 0);
    frame(); // Loading temporarily has no manager, but race_flag remains set.
    constexpr uint32_t new_box = 0x1000A000, new_original = 0x1000B000;
    mem().store<uint32_t>(new_box + 0x78, new_original);
    mem().store<uint32_t>(nui_box_global, new_box);
    frame();
    bind(1, 1, other);
    const bool inherited_first = braking(1);
    mem().store<uint32_t>(race_flag_global, 0); frame();
    mem().store<uint32_t>(nui_box_global, box);
    require(!inherited_first, "temporary null manager during Loading must preserve disconnected P2 ownership");
}
void exit_while_manager_missing() {
    braking(1);
    second.reset();
    mem().store<uint32_t>(nui_box_global, 0);
    frame(); // active body is already removed before the real race exit.
    mem().store<uint32_t>(race_flag_global, 0); frame();
    mem().store<uint32_t>(box + 0x78, original);
    mem().store<uint32_t>(nui_box_global, box);
    mem().store<uint32_t>(race_flag_global, 1); frame();
    bind(1, 1, other);
    require(braking(1), "real race exit with null manager must clear ownership before a single-player race");
}
void live_reader_scope() {
    first = {}; second = sfr::GamepadState{};
    first.thumb_lx = -32768; second->thumb_lx = 32767;
    frame();
    consume(1);
    require(consumed_record == other && f(other + 640) > f(other + 644),
            "P2 lean must receive current P2 fields after original object update");
    consume(0);
    require(consumed_record == injected && f(injected + 644) > f(injected + 640),
            "P1 lean must receive current P1 fields after original object update");
    first.buttons = sfr::gamepad_button::a;
    second->buttons = sfr::gamepad_button::b;
    for (unsigned i = 0; i < 6; ++i) {
        frame(); consume(0); consume(1);
        reset_results();
        const auto crouch = invoke("sub_822C8778", 0, 2);
        require(crouch == (i < 5 ? 0u : 1u), "P1 crouch must follow the same owner as lean");
        reset_results();
        require(invoke("sub_822C8778", 1, 3) == 2, "P1 crouch must not reach the P2 lean source");
    }
    require(f(injected + 8) == 1 && mem().load<uint32_t>(other + 268) == 1,
            "live lean readers and gesture detectors must agree on A/B ownership");
    mem().store<uint32_t>(other + 268, 0x579B);
    PPCContext ctx;
    ctx.r3.u64 = objects + 16;
    auto found = hooks().find("sub_82918418");
    if (found != hooks().end()) found->second(ctx, mem().base());
    else __imp__sub_82918418(ctx, mem().base());
    require(ctx.r3.u32 == other && mem().load<uint32_t>(other + 268) == 0x579B,
            "shared reader outside live race consumer must not patch arbitrary records");
    consumer_throws = true;
    try { consume(1); } catch (const std::runtime_error&) {}
    consumer_throws = false;
    ctx.r3.u64 = objects + 16;
    if (found != hooks().end()) found->second(ctx, mem().base());
    else __imp__sub_82918418(ctx, mem().base());
    require(mem().load<uint32_t>(other + 268) == 0x579B,
            "an exceptional guest exit must restore the reader scope");
}
void independent_actions() {
    first = {}; second = sfr::GamepadState{}; frame();
    first.buttons = sfr::gamepad_button::a; frame();
    first.buttons = 0; second->buttons = sfr::gamepad_button::a; frame();
    reset_results(); require(invoke("sub_822C9050", 0) == 1, "P1 release must jump");
    reset_results(); require(invoke("sub_822C9050", 1) == 2, "P1 release must not jump P2");
    second->buttons = 0; frame();
    reset_results(); require(invoke("sub_822C9050", 1) == 1, "P2 release must jump independently");
    reset_results(); require(invoke("sub_822C9050", 0) == 2, "P2 release must not jump P1");
    first.thumb_lx = 32767; second->thumb_lx = 32767; frame();
    reset_results(); invoke("sub_822C9938", 0, 0); invoke("sub_822C9938", 1, 1);
    first.thumb_lx = 0; first.thumb_ly = 32767; frame();
    reset_results(); invoke("sub_822C9938", 0, 0);
    require(std::fabs(f(selected + 56) - 0.5f) < 0.001f, "P1 trick must use P1 stick");
    reset_results(); invoke("sub_822C9938", 1, 1);
    require(f(selected + 56) == 0, "P1 trick must not move P2 trick");
}
void lifecycle() {
    require(braking(0) && !braking(1), "establish both mappings");
    mem().store<uint32_t>(race_flag_global, 0); frame();
    require(mem().load<uint32_t>(box + 0x78) == original, "race exit must restore original body");
    require(invoke("sub_822C9BF0", 0) == 99 && original_calls, "outside a race detectors must delegate");
    bind(0, 0, other); bind(1, 1, injected);
    mem().store<uint32_t>(race_flag_global, 1); frame();
    require(!braking(0) && braking(1), "reentry must resolve reused sources afresh");
}
void side() {
    reset_results();
    require(invoke("sub_822CA6B0", 0) == 1, "P1 B must preserve Side");
    reset_results();
    require(invoke("sub_822CA6B0", 1) == 2, "P1 B must not cause P2 Side");
    first.buttons = sfr::gamepad_button::a; frame();
    reset_results();
    require(invoke("sub_822CA6B0", 0) == 2, "A must not produce Side and suppress crouch/jump");
}
void camera_detectors() {
    camera_active = true;
    first.buttons = sfr::gamepad_button::a | sfr::gamepad_button::b;
    frame();
    for (const char* detector_name : {"sub_822CA518", "sub_822C9938", "sub_822CB9B8"}) {
        reset_results();
        const auto before = original_calls;
        require(invoke(detector_name, 0) == 99 && original_calls == before + 1,
                "camera P1 must delegate each gesture, including the special grouped route, to its original detector");
        require(mem().load<uint32_t>(selected + 4) == 0x20 && mem().load<uint32_t>(selected + 20) == 0x8,
                "ignored P1 pad must not add gesture result bits before camera detection");
    }
}
void camera_motion_controls() {
    camera_active=true;first={};second=sfr::GamepadState{};
    // Actual game trace: +4 is 1 during normal play, not 1/60 seconds.
    constexpr uint32_t title_clock=0x1000D000, title_step=0x1000D100;
    mem().store<uint32_t>(0x83E516A0,title_clock);
    mem().store<uint32_t>(title_clock+24,title_step);
    mem().store<uint32_t>(title_step+4,std::bit_cast<uint32_t>(1.f));
    mem().store<uint32_t>(title_step+40,std::bit_cast<uint32_t>(1.f));
    const auto posture=[&](float side,float pitch,float height) {
        for(auto [offset,point]:{std::pair{0u,std::array{0.f,0.f,-2.5f}},
            std::pair{32u,std::array{-side,.5f,-2.5f+pitch}},
            std::pair{224u,std::array{-.1f,-height,-2.5f}},
            std::pair{288u,std::array{.1f,-height,-2.5f}}})
            for(uint32_t a=0;a<3;++a)mem().store<uint32_t>(original+offset+a*4,std::bit_cast<uint32_t>(point[a]));
        for(int i=0;i<60;++i)frame();
    };
    posture(0,.1f,.8f);consume(0);
    require(f(injected+640)==1 && f(injected+644)==1,"camera missing depth data must not become a rightward lean");
    posture(-.3f,.1f,.8f);consume(0);
    require(f(injected+644)>f(injected+640),"camera left tilt must steer left at live reader");
    require(f(injected+32)==.3f && f(injected+4)==0,"camera bridge preserves joint XYZ");
    posture(.3f,.1f,.8f);consume(0);
    require(f(injected+640)>f(injected+644),"camera right tilt must steer right at live reader");
    posture(0,.1f,.8f);reset_results();
    require(invoke("sub_822C9A80",0)!=1,"neutral camera pose must not Boost");
    posture(0,.4f,.8f);reset_results();
    require(invoke("sub_822C9A80",0)==1 && (mem().load<uint32_t>(selected+4)&0x8000),"deliberate camera lean triggers Boost");
    posture(0,.4f,.5f);reset_results();
    require(invoke("sub_822C8778",0)==1,"camera compression crouches");
    require(invoke("sub_822CA6B0",0)==2,"camera crouch must not be filtered out by Side");
    require(invoke("sub_822C9A80",0)!=1,"camera crouch cannot Boost");
    reset_results();require(invoke("sub_822C8778",1)!=1,"camera crouch never reaches P2");
    mem().store<uint32_t>(original+228,std::bit_cast<uint32_t>(-.8f));
    mem().store<uint32_t>(original+292,std::bit_cast<uint32_t>(-.8f));
    int jumps=0;
    for(int i=0;i<60;++i){frame();reset_results();if(invoke("sub_822C9050",0)==1)++jumps;}
    require(jumps==1,"camera recovery emits exactly one jump at detector boundary");
    posture(0,.1f,.8f);
    mem().store<uint32_t>(original+40,std::bit_cast<uint32_t>(-2.0f));
    frame(); // One new forward observation, followed by a stalled camera.
    for(int i=0;i<30;++i){camera_now+=16666667;invoke("sub_82438930");}
    reset_results();
    require(invoke("sub_822C9A80",0)!=1 && invoke("sub_822C9050",0)!=1,
            "manager ticks with an unchanged camera generation cannot produce Boost or jump");
}
void camera_brake_facing() {
    camera_active=true;first={};
    const auto point=[&](uint32_t offset,float x,float y,float z) {
        for(auto [a,v]:{std::pair{0u,x},std::pair{4u,y},std::pair{8u,z}})
            mem().store<uint32_t>(original+offset+a,std::bit_cast<uint32_t>(v));
    };
    point(0,0,0,-2.5f);point(32,0,.5f,-2.4f);
    point(224,-.1f,-.8f,-2.5f);point(288,.1f,-.8f,-2.5f);
    point(64,.18f,.5f,-2.5f);point(128,-.18f,.5f,-2.5f);
    point(192,.125f,0,-2.5f);point(256,-.125f,0,-2.5f);
    frame();reset_results();
    require(invoke("sub_822CA6B0",0)==2 && invoke("sub_822C9BF0",0)==2,"uncalibrated camera cannot brake");
    for(int i=0;i<60;++i)frame();reset_results();
    require(invoke("sub_822CA6B0",0)==99 && invoke("sub_822C9BF0",0)==99,
            "qualified frontal camera stance preserves original Side and brake detectors");
    point(192,.02f,0,-2.375f);point(256,-.02f,0,-2.625f);
    frame();reset_results();
    require(invoke("sub_822CA6B0",0)==2 && invoke("sub_822C9BF0",0)==2,
            "pelvis still sideways must suppress false frontal knee brake");
}
void camera_wave_priority() {
    camera_active=true;first={};second=sfr::GamepadState{};
    original_kick_bits=0x1000000; // Would fire if the original detector were allowed through.
    const auto point=[&](uint32_t offset,float x,float y,float z) {
        for(auto [a,v]:{std::pair{0u,x},std::pair{4u,y},std::pair{8u,z}})
            mem().store<uint32_t>(original+offset+a,std::bit_cast<uint32_t>(v));
    };
    point(0,0,0,-2.5f);point(32,0,.5f,-2.4f);
    point(224,-.1f,-.8f,-2.5f);point(288,.1f,-.8f,-2.5f);
    point(64,.18f,.5f,-2.5f);point(128,-.18f,.5f,-2.5f);
    point(192,.125f,0,-2.5f);point(256,-.125f,0,-2.5f);
    point(80,.25f,.28f,-2.5f);point(96,.25f,.1f,-2.4f);
    point(144,-.25f,.28f,-2.5f);point(160,-.25f,.1f,-2.4f);
    for(int i=0;i<60;++i)frame();
    for(int i=0;i<45;++i) {
        point(32,0,.5f,-2.1f);point(160,-.25f+.18f*std::sin(i*.4f),.35f,-2.3f);frame();reset_results();
        require(invoke("sub_822C9A80",0)!=1 && invoke("sub_822CAF48",0)!=1 &&
                invoke("sub_822CA6B0",0)==2 && invoke("sub_822C9BF0",0)==2,
                "camera wave must suppress both Boost routes, Side and brake");
        mem().store<uint32_t>(detector+72,0x0156789A);
        require(invoke("sub_822CA518",0)==2 && mem().load<uint32_t>(detector+72)==0x0056789A,
                "wave clears Kick byte state without damaging adjacent bytes");
        mem().store<uint32_t>(detector+72,2);
        require(invoke("sub_822CBD28",0)==2 && mem().load<uint32_t>(detector+72)==0,
                "wave clears the entire big-endian Dash word state");
        require((mem().load<uint32_t>(selected+4)&0x1C00000)==0,
                "waving cannot gain kick bits or the shared kick priority flag");
        require(invoke("sub_822CC590",0)==99,"wave must preserve original throw detector");
    }
    second->buttons=sfr::gamepad_button::b;frame();reset_results();
    require(invoke("sub_822C9BF0",1)==1,"P1 waving cannot inhibit P2 brake");
    original_kick_bits=0;
}
void camera_kick_balance() {
    camera_active=true;first={};second=sfr::GamepadState{};
    const auto point=[&](uint32_t offset,float x,float y,float z) {
        for(auto [a,v]:{std::pair{0u,x},std::pair{4u,y},std::pair{8u,z}})
            mem().store<uint32_t>(original+offset+a,std::bit_cast<uint32_t>(v));
    };
    point(0,0,0,-2.5f);point(32,0,.5f,-2.4f);
    point(224,-.1f,-.8f,-2.5f);point(288,.1f,-.8f,-2.5f);
    point(208,-.1f,-.4f,-2.5f);point(272,.1f,-.4f,-2.5f);
    point(64,.18f,.5f,-2.5f);point(128,-.18f,.5f,-2.5f);
    point(192,-.125f,0,-2.5f);point(256,.125f,0,-2.5f);
    point(80,.25f,.28f,-2.5f);point(96,.25f,.1f,-2.4f);
    point(144,-.25f,.28f,-2.5f);point(160,-.25f,.1f,-2.4f);
    for(int i=0;i<60;++i)frame();
    point(144,-.25f,.55f,-2.5f);point(160,-.25f,.8f,-2.4f); // Balance with a raised hand.
    point(288,.1f,-.8f,-2.38f);point(272,.1f,-.4f,-2.44f);
    for(int i=0;i<5;++i)frame();
    for(const char* name:{"sub_822CA518","sub_822CBD28"}) {
        original_kick_bits=0x800000;reset_results();
        require(invoke(name,0)==0 && (mem().load<uint32_t>(selected+4)&0xC00000)==0xC00000,
                "confirmed foot sweep preserves original Ready despite balancing hands");
        original_kick_bits=0x1000000;reset_results();
        require(invoke(name,0)==1 && (mem().load<uint32_t>(selected+4)&0x1400000)==0x1400000,
                "confirmed foot sweep permits original kick release despite balancing hands");
        require(invoke("sub_822C9BF0",0)==2,"balancing hands must still suppress false braking");
    }
    point(288,.1f,-.8f,-2.5f);point(272,.1f,-.4f,-2.5f);
    for(int i=0;i<90;++i)frame();reset_results();
    require(invoke("sub_822CA518",0)==2 && (mem().load<uint32_t>(selected+4)&0x1C00000)==0,
            "expired foot evidence restores wave protection without releasing a kick");
    original_kick_bits=0;
}
void camera_kick_priority() {
    camera_active=true;first={};frame();
    // Original group 822C5BB0 drops kick/brake bits unless the shared
    // 0x400000 priority bit is present. It suppresses charge/jump otherwise.
    const auto filtered=[](uint32_t f) {return f&((f&0x400000)?~0x1203u:~0x1800100u);};
    for(const char* name:{"sub_822CA518","sub_822CBD28"}) {
        for(uint32_t phase:{0x800000u,0x1000000u}) {
            reset_results();original_kick_bits=phase;
            const auto result=invoke(name,0);
            require(result==(phase==0x1000000?1u:0u),"camera retains original kick prepare/release return value");
            invoke("sub_822CA6B0",0);invoke("sub_822C9BF0",0);
            const auto bits=mem().load<uint32_t>(selected+4);
            require((filtered(bits)&phase)==phase,"recognized camera kick must survive the original group filter");
            require((bits&0x100)==0,"kick priority cannot synthesize braking");
        }
        original_kick_bits=0;reset_results();invoke(name,0);
        require((mem().load<uint32_t>(selected+4)&0x400000)==0,"no kick cannot synthesize a priority flag");
    }
    original_kick_bits=0x1000000;reset_results();invoke("sub_822CA518",1);
    require((mem().load<uint32_t>(selected+4)&0x1C00000)==0,"camera kick cannot leak into P2");
    original_kick_bits=0;
}
void camera_upper_throw() {
    camera_active=true;first={};second=sfr::GamepadState{};
    const auto point=[&](uint32_t offset,float x,float y,float z) {
        for(auto [a,v]:{std::pair{0u,x},std::pair{4u,y},std::pair{8u,z}})
            mem().store<uint32_t>(original+offset+a,std::bit_cast<uint32_t>(v));
    };
    point(0,0,0,-2.5f);point(32,0,.5f,-2.4f);
    point(224,-.1f,-.8f,-2.5f);point(288,.1f,-.8f,-2.5f);
    point(64,.18f,.5f,-2.5f);point(128,-.18f,.5f,-2.5f);
    point(192,.125f,0,-2.5f);point(256,-.125f,0,-2.5f);
    point(80,.25f,.28f,-2.5f);point(96,.25f,.1f,-2.4f);
    point(144,-.25f,.28f,-2.5f);point(160,-.25f,.1f,-2.4f);
    for(int i=0;i<60;++i)frame();
    const auto prepare=[&] {
        point(144,-.25f,.38f,-2.5f);point(160,-.25f,.6f,-2.22f);frame();
    };
    const auto release=[&] {point(160,-.25f,.38f,-2.1f);frame();};
    const auto clear=[&] {reset_results();mem().store<uint32_t>(selected+20,0x40);};
    prepare();release();clear();
    invoke("sub_822CC590",1);
    require(mem().load<uint32_t>(selected+20)==0x40,"P2 does not receive P1 upper throw");
    mem().store<uint32_t>(detector+72,1);mem().store<uint32_t>(detector+76,1);
    require(invoke("sub_822CC590",0)==1 && mem().load<uint32_t>(selected+20)==0x48,
            "camera upper throw reaches original item flag and preserves other bits");
    require(mem().load<uint32_t>(detector+72)==0 && mem().load<uint32_t>(detector+76)==0,
            "fallback clears both original arm words");
    clear();require(invoke("sub_822CC590",0)==99 && mem().load<uint32_t>(selected+20)==0x40,
            "second detector call cannot resend a consumed upper throw");
    prepare();release();original_overthrow_result=1;
    invoke("sub_822CC590",0);original_overthrow_result=99;clear();
    require(invoke("sub_822CC590",0)==99 && mem().load<uint32_t>(selected+20)==0x40,
            "original successful throw consumes fallback without a second event");
    prepare();camera_active=false;frame();camera_active=true;release();clear();
    require(invoke("sub_822CC590",0)==99,"camera ownership loss discards prepared upper throw");
}
void camera_actor_trace() {
    camera_active=true;
    constexpr uint32_t actor=box+0xA000,owner=box+0xC000,res=box+0xE000;
    constexpr uint32_t ring_entries=box+0xE100,state=box+0xE500,animation=box+0xE700,data=box+0xE800;
    mem().store<uint32_t>(actor+1680,owner);
    // The actor field is deliberately not a pointer: owner indirection matters.
    mem().store<uint32_t>(actor+3208,0x41980000);
    mem().store<uint32_t>(owner+3220,res);mem().store<uint32_t>(owner+3208,animation);
    mem().store<uint32_t>(owner+3212,state);mem().store<uint32_t>(animation+12,data);
    mem().store<uint32_t>(data+44,19);mem().store<uint32_t>(state+400,6);
    mem().store<uint32_t>(res+16,ring_entries);mem().store<uint32_t>(res+24,0);mem().store<uint32_t>(res+28,1);
    mem().store<uint32_t>(ring_entries+4,0x7000);mem().store<uint32_t>(ring_entries+84+4,2);
    mem().store<uint32_t>(res+36,2);mem().store<uint32_t>(res+40,0xFFFFFFFF);
#ifdef _WIN32
    _putenv_s("SFR_CAMERA_RACE_TRACE","1");
#else
    setenv("SFR_CAMERA_RACE_TRACE","1",1);
#endif
    std::ostringstream log;auto* old=std::cerr.rdbuf(log.rdbuf());
    PPCContext ctx;ctx.r3.u64=actor;
    try {hooks().at("sub_822B60F8")(ctx,mem().base());}
    catch(...) {std::cerr.rdbuf(old);throw;}
    std::cerr.rdbuf(old);
    require(ctx.r3.u32==99 && log.str().find("written=0x7000 flags=0x2")!=std::string::npos &&
            log.str().find("anim=19 state=6")!=std::string::npos,
            "actor diagnostic follows owner and distinct writer/consumer entries without changing guest result");
}
void camera_body_fields() {
    camera_active = true;
    first.buttons = sfr::gamepad_button::a | sfr::gamepad_button::b;
    first.thumb_lx = -32768;
    constexpr uint32_t brake_marker = 0x1357, lean_marker = 0x3F400000;
    mem().store<uint32_t>(original + 268, brake_marker);
    mem().store<uint32_t>(original + 644, lean_marker);
    frame();
    require(mem().load<uint32_t>(injected + 268) == brake_marker &&
            mem().load<uint32_t>(injected + 644) == lean_marker,
            "camera P1 manager must preserve the title body fields instead of writing pad controls");
    bind(4, 4, original);
    for (unsigned source : {0u, 4u}) {
        const uint32_t record = source == 0 ? injected : original;
        reset_results();
        require(invoke("sub_822C9BF0", source) == 2, "unqualified camera stance must gate brake across body aliases");
        require(mem().load<uint32_t>(record + 268) == brake_marker && mem().load<uint32_t>(record + 644) == lean_marker,
                "resolving the camera P1 detector source must not write pad fields");
        consume(source);
        require(consumed_record == record && f(record + 640)>=1 && f(record+644)>=1 &&
                mem().load<uint32_t>(record + 268) == brake_marker,
                "camera live reader fills only positive lean ratios and preserves joint fields");
    }
}
void camera_keeps_second_pad() {
    camera_active = true;
    first = {};
    second->buttons = sfr::gamepad_button::b;
    second->thumb_lx = 32767;
    frame();
    const auto before = original_calls;
    require(braking(1) && original_calls == before, "camera P1 must leave P2 braking on its own pad");
    consume(1);
    require(consumed_record == other && mem().load<uint32_t>(other + 268) == 1 && f(other + 640) > f(other + 644),
            "camera P1 must leave P2 live body fields on its own pad");
    second->buttons = sfr::gamepad_button::a; frame();
    second->buttons = 0; frame();
    reset_results();
    require(invoke("sub_822C9050", 1) == 1, "P2 A release must still jump while camera controls P1");
}
void camera_returns_to_pad() {
    first.buttons = sfr::gamepad_button::a; frame();
    camera_active = true;
    frame(); // A remains held but camera ownership must clear the pad history.
    camera_active = false;
    first.buttons = sfr::gamepad_button::b;
    first.thumb_lx = -32768;
    frame();
    require(mem().load<uint32_t>(injected + 268) == 1,
            "returning to P1 pad must restore manager body overrides");
    reset_results();
    require(invoke("sub_822C9050", 0) == 2 && (mem().load<uint32_t>(selected + 4) & 0x200) == 0,
            "returning from camera must not jump from an ignored A release or pre-camera pad history");
    require(braking(0), "returning to P1 pad must restore detector overrides");
    consume(0);
    require(f(injected + 644) > f(injected + 640), "returning to P1 pad must restore live-reader lean overrides");
    first.buttons = sfr::gamepad_button::a; frame();
    first.buttons = 0; frame();
    reset_results();
    require(invoke("sub_822C9050", 0) == 1, "a new P1 pad A press and release must jump after camera handoff");
}
}
int main() {
    using namespace harness;
    sfr::GuestMemory memory;
    sfr::active_memory = &memory;
    memory.map(box, 0x10000);
    memory.map(0x83E50000, 0x4000);
    memory.store<uint32_t>(nui_box_global, box);
    memory.store<uint32_t>(vtable + 4, reader);
    memory.store<uint32_t>(results, entries);
    memory.store<uint32_t>(results + 8, 1);
    unsigned failures = 0;
    const std::pair<const char*, void (*)()> cases[] = {
        {"two objects per player", baseline}, {"source reuse", source_reuse},
        {"record rebind", record_rebind}, {"zero then valid", zero_then_valid},
        {"original alias", original_alias}, {"disconnect", disconnect},
        {"disconnect held actions", disconnect_held_actions},
        {"manager body rebind", body_rebind}, {"manager replacement", manager_replacement},
        {"manager record replacement", manager_record_replacement},
        {"retired source", retired_source}, {"replacement after disconnect", replacement_after_disconnect},
        {"missing manager after disconnect", missing_manager_after_disconnect},
        {"exit while manager missing", exit_while_manager_missing},
        {"live reader scope", live_reader_scope},
        {"independent actions", independent_actions},
        {"exit and reentry", lifecycle}, {"Side override", side},
        {"camera motion controls", camera_motion_controls}, {"camera waving priority", camera_wave_priority}, {"camera frontal brake qualification", camera_brake_facing}, {"camera original detectors", camera_detectors}, {"camera body fields", camera_body_fields},
        {"camera upper throw", camera_upper_throw},
        {"camera kick group priority", camera_kick_priority},
        {"camera kick balance", camera_kick_balance},
        {"camera actor trace", camera_actor_trace},
        {"camera with independent P2", camera_keeps_second_pad}, {"camera return to pad", camera_returns_to_pad}
    };
    for (auto [name, test] : cases) {
        try { begin(); test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    return failures ? 1 : 0;
}
