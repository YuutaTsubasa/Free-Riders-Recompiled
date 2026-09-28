#include <utility>
#include <bit>
#include "ppc_recomp_shared.h"
#include "diagnostic_hooks.h"
#include "nui_race.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace harness {
using Hook = void (*)(PPCContext&, uint8_t*);
auto& hooks() { static std::unordered_map<std::string, Hook> value; return value; }
bool register_hook(const char* name, Hook hook) { hooks().emplace(name, hook); return true; }
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
sfr::GamepadState input;
constexpr uint32_t box = 0x10000000, original = 0x10001000;
constexpr uint32_t results = 0x10003000, entries = 0x10003100;
constexpr uint32_t selected = entries + 84, detector = 0x10004000;
constexpr uint32_t nui_box_global = 0x83E52F88, race_flag_global = 0x83E52F8C;
constexpr uint32_t existing_primary = 0x20, existing_secondary = 0x8;
unsigned original_side_calls = 0, manager_calls = 0;
bool sensor_body = false;
uint64_t kinect_sequence = 0;
}

// Compile the real hooks, capturing their production registration names so a
// missing override reaches the original boundary just as it does in the game.
#undef SFR_HOOK
#define SFR_HOOK(name) PPC_FUNC(name); \
    static const bool name##_registered = harness::register_hook(#name, name); PPC_FUNC(name)
#include "../src/nui_race_hooks.cpp"

namespace sfr {
GuestMemory* active_memory = nullptr;
bool camera_motion_active() { return false; }
uint64_t camera_pose_generation() { return 0; }
uint64_t camera_motion_clock_ns() { return 0; }
std::optional<GamepadState> second_player_pad() { return std::nullopt; }
GamepadState nui_gamepad() { return harness::input; }
bool nui_body_from_sensor() { return harness::sensor_body; }
uint64_t kinect_frame_generation() { return harness::kinect_sequence; }
void enter_function_observed(PPCContext&, const char*, uint32_t) {}
void guest_checkpoint_permit() {}
void call_indirect(PPCContext&, uint8_t*, uint32_t) {
    throw std::runtime_error("unexpected indirect guest call");
}
}

PPC_FUNC(__imp__sub_822C6200) {}
PPC_FUNC(__imp__sub_82918418) { ctx.r3.u64 = sfr::active_memory->load<uint32_t>(ctx.r3.u32 + 4); }
PPC_FUNC(__imp__sub_82438930) { ++harness::manager_calls; }

// Fixture for the original Side detector's observed result with the copied
// race pose: tracked hands recognize Side. This is a guest-code boundary,
// not an implementation of the replacement detector. The production manager
// and RaceInput decide whether that pose has tracked hands.
PPC_FUNC(__imp__sub_822CA6B0) {
    ++harness::original_side_calls;
    auto& m = *sfr::active_memory;
    const uint32_t record = m.load<uint32_t>(harness::box + 0x78);
    const uint32_t e = m.load<uint32_t>(ctx.r5.u32) + m.load<uint32_t>(ctx.r5.u32 + 8) * 84;
    if (m.load<uint32_t>(record + 740) == 2 && m.load<uint32_t>(record + 756) == 2) {
        m.store<uint32_t>(e + 4, m.load<uint32_t>(e + 4) | 0x400000);
        ctx.r3.u64 = 1;
    } else {
        m.store<uint32_t>(e + 20, m.load<uint32_t>(e + 20) | 0x2000);
        ctx.r3.u64 = 2;
    }
}

#define UNUSED_ORIGINAL(address) PPC_FUNC(__imp__sub_##address) { \
    throw std::runtime_error("unexpected original detector " #address); }
UNUSED_ORIGINAL(822C9050)
UNUSED_ORIGINAL(822B60F8)
UNUSED_ORIGINAL(822C8778)
UNUSED_ORIGINAL(822CB840)
UNUSED_ORIGINAL(822C8650)
UNUSED_ORIGINAL(822C9180)
UNUSED_ORIGINAL(822C9A80)
UNUSED_ORIGINAL(822CAF48)
UNUSED_ORIGINAL(822C9BF0)
UNUSED_ORIGINAL(822CB0B8)
UNUSED_ORIGINAL(822CA518)
UNUSED_ORIGINAL(822CBD28)
UNUSED_ORIGINAL(822CA810)
UNUSED_ORIGINAL(822CBF30)
UNUSED_ORIGINAL(822CAC90)
UNUSED_ORIGINAL(822C9938)
UNUSED_ORIGINAL(822CC2D0)
UNUSED_ORIGINAL(822CC590)
UNUSED_ORIGINAL(822CCD98)
UNUSED_ORIGINAL(822CD068)
UNUSED_ORIGINAL(822CDDB0)
UNUSED_ORIGINAL(822CC8F0)
UNUSED_ORIGINAL(822CDEE0)
UNUSED_ORIGINAL(822CD638)
UNUSED_ORIGINAL(822CD960)
UNUSED_ORIGINAL(822CD3F0)
UNUSED_ORIGINAL(822CE118)
UNUSED_ORIGINAL(822CB9B8)
UNUSED_ORIGINAL(822B72E0)
PPC_FUNC(sub_822C8958) { throw std::runtime_error("unexpected grouped detector"); }

namespace harness {
uint32_t invoke(const char* name) {
    PPCContext ctx;
    ctx.r3.u64 = detector;
    ctx.r5.u64 = results;
    auto found = hooks().find(name);
    if (found != hooks().end()) found->second(ctx, sfr::active_memory->base());
    else if (std::string(name) == "sub_822CA6B0")
        __imp__sub_822CA6B0(ctx, sfr::active_memory->base());
    else throw std::runtime_error(std::string("unregistered hook ") + name);
    return ctx.r3.u32;
}

void frame(uint16_t buttons, int16_t left_y = 0) {
    input = {};
    input.buttons = buttons;
    input.thumb_ly = left_y;
    invoke("sub_82438930");
    auto& m = *sfr::active_memory;
    m.store<uint32_t>(selected + 4, existing_primary);
    m.store<uint32_t>(selected + 20, existing_secondary);
}

uint32_t primary() { return sfr::active_memory->load<uint32_t>(selected + 4); }
uint32_t secondary() { return sfr::active_memory->load<uint32_t>(selected + 20); }

// Observed suppression in the title's group detector (822C5BB0): a Side
// result removes crouch and jump. Keeping this rule is part of the contract.
uint32_t filtered() { const auto bits = primary(); return bits & ((bits & 0x400000) ? ~0x1203u : ~0u); }

void conflicting_action(uint16_t button, const char* action) {
    frame(0);
    frame(sfr::gamepad_button::a | button);
    invoke("sub_822CB840");
    invoke(action);
    invoke("sub_822CA6B0");
    require((primary() & 0x1000) && (primary() & 0x400000), "conflicting action must accompany charge");
    require((filtered() & 0x1000) == 0, "brake and kick must keep priority over charge");

    // Keep braking while releasing A, or release A and X together to kick.
    frame(button == sfr::gamepad_button::b ? button : 0);
    invoke("sub_822C9050");
    invoke(action);
    invoke("sub_822CA6B0");
    require((primary() & 0x200) && (primary() & 0x400000), "conflicting action must accompany jump");
    require((filtered() & 0x200) == 0, "brake and kick must keep priority over jump");
}

void crouch_jump(int16_t left_y) {
    frame(0);
    invoke("sub_822C8778"); // reset the detector's accumulated hold
    for (unsigned held = 1; held <= 6; ++held) {
        frame(sfr::gamepad_button::a, left_y);
        const uint32_t record = sfr::active_memory->load<uint32_t>(box + 0x78);
        require(sfr::active_memory->load<uint32_t>(record + 740) == 2 &&
                sfr::active_memory->load<uint32_t>(record + 756) == 2,
                "A must preserve tracked hands for other body consumers");
        const auto crouch = invoke("sub_822C8778");
        require(crouch == (held < 6 ? 0u : 1u), "crouch must recognize after six held frames");
        require(invoke("sub_822C9050") == 2, "held A must not jump");
        require(invoke("sub_822CA6B0") == 2, "A must not be classified as Side");
        require((primary() & 0x400000) == 0, "A must not produce Side's suppressing bit");
        require(secondary() == (existing_secondary | 0x2000), "inactive Side must preserve secondary bits");
        if (held == 6) require((filtered() & 0x1000) != 0, "charged crouch must survive the game filter");
        require((primary() & existing_primary) != 0, "detectors must preserve existing primary bits");
    }
    frame(0, left_y);
    require(invoke("sub_822C8778") == 2, "release must end crouch");
    require(invoke("sub_822C9050") == 1, "release must jump");
    require(invoke("sub_822CA6B0") == 2, "release must not become Side");
    require((filtered() & 0x200) != 0, "release jump must survive the game filter");
    frame(0);
    require(invoke("sub_822C9050") == 2 && (filtered() & 0x200) == 0, "jump release must last one frame");
}

void run() {
    sfr::GuestMemory m;
    sfr::active_memory = &m;
    m.map(box, 0x5000);
    // Include the title globals with alignment valid on 16 KiB-page hosts.
    m.map(0x83E50000, 0x4000);
    m.store<uint32_t>(nui_box_global, box);
    m.store<uint32_t>(box + 0x78, original);
    m.store<uint32_t>(race_flag_global, 1);
    m.store<uint32_t>(results, entries);
    m.store<uint32_t>(results + 8, 1); // catch writes to the wrong 84-byte entry
    m.store<uint32_t>(original + 700, 0x12345678);

    crouch_jump(0);
    crouch_jump(-32768); // tracked hands from stick-down must not restore the bug
    const uint32_t record = m.load<uint32_t>(box + 0x78);
    require(record != original && m.load<uint32_t>(record + 700) == 0x12345678,
            "manager must preserve copied skeleton geometry");
    require(original_side_calls == 0, "pad racing must not delegate Side to the skeleton");

    frame(sfr::gamepad_button::b, -32768);
    require(invoke("sub_822CA6B0") == 1 && primary() == (existing_primary | 0x400000), "B must still recognize Side");
    require(secondary() == existing_secondary, "active Side must not report its opposite");
    require(invoke("sub_822C9BF0") == 1 && primary() == (existing_primary | 0x400100) &&
            m.load<uint8_t>(selected + 80) == 100, "B braking and strength must be preserved");

    for (const char* kick : {"sub_822CA518", "sub_822CBD28"}) {
        frame(0);
        frame(sfr::gamepad_button::x);
        require(invoke(kick) == 0, "X press must begin the kick");
        require(invoke("sub_822CA6B0") == 2 && primary() == (existing_primary | 0xC00000),
                "inactive Side must preserve X press bits, including shared 0x400000");
        frame(sfr::gamepad_button::x);
        require(invoke(kick) == 2 && primary() == existing_primary, "held X must not repeat its edge");
        frame(0);
        require(invoke(kick) == 1, "X release must fire the kick");
        require(invoke("sub_822CA6B0") == 2 && primary() == (existing_primary | 0x1400000),
                "inactive Side must preserve X release bits");
    }
    conflicting_action(sfr::gamepad_button::b, "sub_822C9BF0");
    conflicting_action(sfr::gamepad_button::x, "sub_822CA518");
    conflicting_action(sfr::gamepad_button::x, "sub_822CBD28");
    require(m.load<uint32_t>(entries + 4) == 0 && m.load<uint32_t>(entries + 20) == 0,
            "detectors must write only the selected result entry");

    m.store<uint32_t>(race_flag_global, 0);
    m.store<uint32_t>(original + 740, 2);
    m.store<uint32_t>(original + 756, 2);
    frame(0);
    require(m.load<uint32_t>(box + 0x78) == original, "leaving race must restore original body");
    require(invoke("sub_822CA6B0") == 1 && original_side_calls == 1 && (primary() & 0x400000),
            "outside pad racing Side must delegate to the original detector");
    require(manager_calls > 0, "manager must continue invoking the original update");

    // A real Kinect tracks the player: the race reads the title's own body
    // record and detectors, whatever the pad does.
    sensor_body = true;
    m.store<uint32_t>(race_flag_global, 1);
    frame(sfr::gamepad_button::b, -32768);
    require(m.load<uint32_t>(box + 0x78) == original, "a sensor's body must stay the title's record");
    require(invoke("sub_822CA6B0") == 1 && original_side_calls == 2,
            "with a sensor the original detectors must run");

    // No depth image reaches the title, so a sensor's lean pair comes from
    // the body: upright after the stance calibration, then the torso's roll.
    const auto put = [&](uint32_t offset, float x, float y, float z) {
        m.store<uint32_t>(original + offset, std::bit_cast<uint32_t>(x));
        m.store<uint32_t>(original + offset + 4, std::bit_cast<uint32_t>(y));
        m.store<uint32_t>(original + offset + 8, std::bit_cast<uint32_t>(z));
    };
    const auto lean_pair = [&] {
        return std::pair{std::bit_cast<float>(m.load<uint32_t>(original + 640)),
                         std::bit_cast<float>(m.load<uint32_t>(original + 644))};
    };
    put(0, 0, 0, 2.2f);        // hip centre
    put(32, 0, 0.5f, 2.2f);    // shoulder centre
    put(224, -0.1f, -0.8f, 2.2f);
    put(288, 0.1f, -0.8f, 2.2f);
    for (int i = 0; i < 60; ++i) { ++kinect_sequence; frame(0); }
    require(lean_pair() == std::pair{1.f, 1.f}, "a calibrated upright sensor body leans neither way");
    put(32, -0.3f, 0.5f, 2.2f);  // shoulders over one side of the hips
    for (int i = 0; i < 60; ++i) { ++kinect_sequence; frame(0); }
    const auto [right, left] = lean_pair();
    require(right > 1.5f && left == 1.f, "a sensor body's roll leans the race one way");
    put(32, 0.3f, 0.5f, 2.2f);
    for (int i = 0; i < 60; ++i) { ++kinect_sequence; frame(0); }
    require(lean_pair().first == 1.f && lean_pair().second > 1.5f, "and the other roll the other way");
    sensor_body = false;
}
}

int main() {
    try {
        harness::run();
        std::cout << "nui race hook tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
