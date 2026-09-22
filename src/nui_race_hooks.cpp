#include "ppc_recomp_shared.h"
#include "diagnostic_hooks.h"
#include "guest_memory.h"
#include "nui_race.h"
#include <bit>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <unordered_map>

// Race controls (see nui_race.h). Gesture detectors are called as
// detect(detector, source, results): source's vtable[1] returns the player's
// body record, results = {entries, ?, index, second index} with 84-byte
// entries whose words +4 and +20 collect the recognized gesture bits. A
// detector returns 1 when it recognized its gesture, 2 when it saw the
// opposite state (e.g. hands not up) and 0 otherwise. The bits each gesture
// reports are the ones the original detectors set.

namespace {
sfr::RaceInput race;
constexpr uint32_t body_address = 0x71730000, body_mapping = 0x2000;
constexpr uint32_t nui_box = 0x83E52F88, race_flag = 0x83E52F8C, time_global = 0x83E516A0;
bool swapped = false;
uint32_t original_body = 0;

sfr::GuestMemory& memory() { return *sfr::active_memory; }
float load_float(uint32_t address) { return std::bit_cast<float>(memory().load<uint32_t>(address)); }
void store_float(uint32_t address, float value) { memory().store<uint32_t>(address, std::bit_cast<uint32_t>(value)); }

// Title frame step: [[83E516A0]+24] holds the elapsed time (+4) and frames (+40).
float frame_seconds() {
    const uint32_t clock = memory().load<uint32_t>(time_global);
    return clock ? load_float(memory().load<uint32_t>(clock + 24) + 4) : 1.0f / 60.0f;
}
float frame_count() {
    const uint32_t clock = memory().load<uint32_t>(time_global);
    return clock ? load_float(memory().load<uint32_t>(clock + 24) + 40) : 1.0f;
}

bool pad_racing() { return swapped; }

// Result entry the detector writes: [results] + [results+8 (or +12)] * 84.
uint32_t entry(uint32_t results, uint32_t index_offset = 8) {
    return memory().load<uint32_t>(results) + memory().load<uint32_t>(results + index_offset) * 84;
}
void set_bits(uint32_t address, uint32_t bits) { memory().store<uint32_t>(address, memory().load<uint32_t>(address) | bits); }
void set_byte(uint32_t address, uint8_t value) { memory().store<uint8_t>(address, value); }

const sfr::RaceBody& body() { return race.body(); }
}

PPC_FUNC_IMPL(__imp__sub_82438930);

// Kinect manager update, once per frame before it handles the players: in a
// race ([83E52F8C] nonzero) P1's body record (KinnectNuiBox+0x78) is ours,
// refreshed from the pad; afterwards the title's record is put back.
SFR_HOOK(sub_82438930) {
    sfr::enter_function(ctx, "sub_82438930", 0x82438930);
    auto& m = memory();
    const uint32_t box = m.load<uint32_t>(nui_box);
    // SFR_NO_RACE_BODY leaves the title's own body record in place.
    static const bool disabled = std::getenv("SFR_NO_RACE_BODY") != nullptr;
    const bool racing = !disabled && m.load<uint32_t>(race_flag) != 0;
    race.update(sfr::nui_gamepad(), frame_seconds());
    if (box && racing) {
        if (m.available(body_address, body_mapping)) {
            m.map(body_address, body_mapping);
            for (uint32_t offset = 0; offset < body_mapping; offset += 4) m.store<uint32_t>(body_address + offset, 0);
        }
        const uint32_t current = m.load<uint32_t>(box + 0x78);
        if (current != body_address) {
            original_body = current;
            m.store<uint32_t>(box + 0x78, body_address);
            std::cerr << "NUI_RACE_BODY installed original=0x" << std::hex << current << std::dec << '\n';
        }
        // Every field the title keeps (pointers, joint positions) is the one
        // it built, refreshed each frame: copying once left the body frozen in
        // the pose it had when the race began, a raised menu cursor hand
        // included. Only the fields below are the pad's.
        if (original_body)
            for (uint32_t offset = 0; offset < sfr::race_body_size; offset += 4)
                m.store<uint32_t>(body_address + offset, m.load<uint32_t>(original_body + offset));
        swapped = true;
        race.write(m, body_address);
        // SFR_RACE_BODY_DUMP=N: every N frames, the record's non-zero floats.
        // The title fills the rest from the skeleton, so this shows what the
        // race reads besides the pad's fields.
        static const uint32_t dump = [] { const char* t = std::getenv("SFR_RACE_BODY_DUMP");
                                          return t ? uint32_t(std::strtoul(t, nullptr, 10)) : 0u; }();
        static uint32_t frames = 0;
        if (dump && frames++ % dump == 0) {
            std::cerr << "NUI_RACE_BODY_FIELDS frame=" << frames;
            for (uint32_t offset = 0; offset < sfr::race_body_size; offset += 4) {
                const uint32_t word = m.load<uint32_t>(body_address + offset);
                const float value = std::bit_cast<float>(word);
                if (word && std::isfinite(value) && std::fabs(value) < 1e6f)
                    std::cerr << ' ' << offset << '=' << value;
                else if (word)
                    std::cerr << ' ' << offset << "=0x" << std::hex << word << std::dec;
            }
            std::cerr << '\n';
        }
    } else if (swapped) {
        if (box && m.load<uint32_t>(box + 0x78) == body_address) m.store<uint32_t>(box + 0x78, original_body);
        swapped = false;
        std::cerr << "NUI_RACE_BODY restored original=0x" << std::hex << original_body << std::dec << '\n';
    }
    __imp__sub_82438930(ctx, base);
}

// One detector override: while the pad drives the race the body decides,
// otherwise the original detector runs.
#define RACE_DETECTOR(address, ...)                                      \
    PPC_FUNC_IMPL(__imp__sub_##address);                                       \
    SFR_HOOK(sub_##address) {                                                  \
        sfr::enter_function(ctx, "sub_" #address, 0x##address);               \
        if (!pad_racing()) { __imp__sub_##address(ctx, base); return; }        \
        const uint32_t detector = ctx.r3.u32, source = ctx.r4.u32, results = ctx.r5.u32; \
        (void)detector; (void)source; (void)results;                           \
        const sfr::RaceBody& b = body();                                       \
        uint32_t result = 0;                                                   \
        __VA_ARGS__                                                            \
        ctx.r3.u64 = result;                                                   \
    }

// Jump: releasing A (after crouching with it). The detector's +88/+92 hold
// the jump direction, -1 on the jump frame and 1 otherwise.
RACE_DETECTOR(822C9050, {
    const bool jump = b.a_released != 0;
    memory().store<uint32_t>(detector + 88, jump ? 0xFFFFFFFFu : 1u);
    memory().store<uint32_t>(detector + 92, jump ? 0xFFFFFFFFu : 1u);
    if (jump) { set_bits(entry(results) + 4, 0x200); result = 1; } else result = 2;
})

// Crouch: A held for six frames.
namespace { std::unordered_map<uint32_t, float> crouch_frames; }
RACE_DETECTOR(822C8778, {
    float& held = crouch_frames[detector];
    if (b.a_held == 0) { held = 0; result = 2; }
    else {
        if (held < 6.0f) held += frame_count();
        if (held >= 6.0f) { set_bits(entry(results) + 4, 0x7000); result = 1; }
    }
})

// Charge: A held.
RACE_DETECTOR(822CB840, {
    if (b.a_held != 0) { set_bits(entry(results) + 4, 0x7000); result = 1; }
})

// Arms up: right stick up.
RACE_DETECTOR(822C8650, {
    if (b.right_y > 0) {
        set_bits(entry(results) + 4, 0x100000C0);
        set_bits(entry(results) + 20, 0x120);
        result = 1;
    } else result = 2;
})

// Flying skill: A held.
RACE_DETECTOR(822C9180, {
    if (b.a_held == 0) result = 2;
    else { set_bits(entry(results) + 4, 0x40000); result = 1; }
})

// Board acceleration (and its second detector): left stick pushed forward.
#define RACE_BOARD_ACCELERATION(address)                                       \
    RACE_DETECTOR(address, {                                                   \
        if (b.left_y >= 0.65f) { set_bits(entry(results) + 4, 0x8000); result = 1; } \
    })
RACE_BOARD_ACCELERATION(822C9A80)
RACE_BOARD_ACCELERATION(822CAF48)

// Brake: B held, harder with the left stick (+80 = strength in percent).
RACE_DETECTOR(822C9BF0, {
    const uint32_t e = entry(results);
    if (b.b_held) {
        set_byte(e + 80, uint8_t(std::fabs(b.left_y) * 100.0f));
        set_bits(e + 4, 0x00400100);
        result = 1;
    } else { set_byte(e + 80, 0); result = 2; }
})

// Grab (poles, rails): B held, full strength.
RACE_DETECTOR(822CB0B8, {
    const uint32_t e = entry(results);
    if (b.b_held) { set_byte(e + 80, 100); set_bits(e + 4, 0x100); result = 1; }
    else { set_byte(e + 80, 0); result = 2; }
})

// Kick dash (and dash): X pressed starts the kick, releasing X fires it.
#define RACE_KICK(address)                                                     \
    RACE_DETECTOR(address, {                                                   \
        if (b.x_pressed != 0) { set_bits(entry(results) + 4, 0x00C00000); result = 0; } \
        else if (b.x_released != 0) { set_bits(entry(results) + 4, 0x01400000); result = 1; } \
        else result = 2;                                                       \
    })
RACE_KICK(822CA518)
RACE_KICK(822CBD28)

// Power skill (and its second detector): LB left side, RB right side.
#define RACE_POWER_SKILL(address)                                              \
    RACE_DETECTOR(address, {                                                   \
        uint32_t bits = 0;                                                     \
        if (b.lb_pressed) bits |= 0x80000;                                     \
        if (b.rb_pressed) bits |= 0x100000;                                    \
        if (bits) { set_bits(entry(results) + 4, bits); result = 1; }         \
    })
RACE_POWER_SKILL(822CA810)
RACE_POWER_SKILL(822CBF30)

// Stance: Y switches between regular and goofy (+72 of the result). The
// source's +96/+100 name the front and back foot joints (title descriptors
// 8218B518 / 8218B4F8) and +104 the stance.
RACE_DETECTOR(822CAC90, {
    if (b.y_pressed != 0) {
        const uint32_t e = entry(results);
        const uint32_t goofy = memory().load<uint32_t>(e + 72) == 0 ? 1 : 0;
        memory().store<uint32_t>(e + 72, goofy);
        memory().store<uint32_t>(source + 104, goofy);
        memory().store<uint32_t>(source + 100, goofy ? 0x8218B518u : 0x8218B4F8u);
        memory().store<uint32_t>(source + 96, goofy ? 0x8218B4F8u : 0x8218B518u);
        set_bits(e + 4, 0x200000);
        result = 2;
    }
})

// Tricks: turning the left stick around turns the trick (+56 of the result
// accumulates half turns, +4 bit 0x800).
namespace { struct Turn { bool started = false; float angle = 0; }; std::unordered_map<uint32_t, Turn> turns; }
RACE_DETECTOR(822C9938, {
    result = 1;
    if (b.left_x != 0 || b.left_y != 0) {
        Turn& turn = turns[detector];
        const float angle = std::fmod(std::atan2(b.left_y, b.left_x) * 57.29578f + 360.0f, 360.0f);
        if (turn.started && angle != turn.angle) {
            float delta = angle - turn.angle;
            if (std::fabs(delta) > 180.0f) delta += delta > 0 ? -360.0f : 360.0f;
            const uint32_t e = entry(results);
            store_float(e + 56, load_float(e + 56) + delta / 180.0f);
            set_bits(e + 4, 0x800);
        }
        turn.started = true;
        turn.angle = angle;
    }
})

// Item and special-gear actions: the right trigger (+20 bits).
#define RACE_TRIGGER(address, bits, ...)                                       \
    RACE_DETECTOR(address, {                                                   \
        if (b.right_trigger > 0) { __VA_ARGS__; set_bits(entry(results) + 20, bits); result = 1; } \
    })
RACE_TRIGGER(822CC2D0, 0x1, {})                                                            // hammer
RACE_TRIGGER(822CC590, 0x8, { set_byte(detector + 72, 255); memory().store<uint32_t>(detector + 76, 1); })  // over throw
RACE_TRIGGER(822CCD98, 0x10, { memory().store<uint32_t>(detector + 76, 1); })             // under swing
RACE_TRIGGER(822CD068, 0x2, { memory().store<uint32_t>(detector + 72, 0); })              // under throw
RACE_TRIGGER(822CDDB0, 0x400, { set_byte(detector + 72, 255); })                          // inflator

// Shake: the right trigger shakes every third of a second while held.
namespace { std::unordered_map<uint32_t, float> shakes; }
RACE_DETECTOR(822CC8F0, {
    if (b.right_trigger == 0) result = 2;
    else {
        float& time = shakes[detector];
        time += frame_seconds();
        if (time >= 1.0f / 3.0f) { time = 0; set_bits(entry(results) + 20, 0x44); result = 1; }
    }
})

// Steam wipe: the right trigger wipes where the right stick points (+76 x,
// +78 y in screen units around the centre).
RACE_DETECTOR(822CDEE0, {
    if (b.right_trigger > 0) {
        set_bits(entry(results) + 20, 0x10000);
        auto move = [&](uint32_t offset, float axis, float limit) {
            const float step = axis * frame_count() * 14.0f;
            const float next = std::clamp(float(int16_t(memory().load<uint16_t>(detector + offset))) - step, -limit, limit);
            memory().store<uint16_t>(detector + offset, uint16_t(int16_t(next)));
        };
        move(78, b.right_y, 640.0f);
        move(76, -b.right_x, 480.0f);
    }
})

// Lever and handle gear: the left stick (+82 / +83 of the result, percent).
RACE_DETECTOR(822CD638, {
    if (b.left_x != 0) {
        const uint32_t e = entry(results);
        set_byte(e + 82, uint8_t(int8_t(b.left_x * 100.0f)));
        if (memory().load<uint8_t>(e + 82)) { set_bits(e + 20, 0x1000); result = 1; }
    }
})
RACE_DETECTOR(822CD960, {
    const uint32_t e = entry(results);
    if (b.left_x == 0) set_byte(e + 83, 0);
    else {
        set_byte(e + 83, uint8_t(int8_t(b.left_x * 100.0f)));
        if (memory().load<uint8_t>(e + 83)) { set_bits(e + 20, 0x4000); result = 1; }
    }
})
// Front curve gear: the lean, on the second result entry (+81).
RACE_DETECTOR(822CD3F0, {
    const uint32_t e = entry(results, 12);
    set_byte(e + 81, uint8_t(int8_t(-b.lean * 100.0f)));
    if (memory().load<uint8_t>(e + 81)) set_bits(e + 20, 0x800);
})

// Swimming: paddling by holding X (strokes every half second).
namespace { std::unordered_map<uint32_t, float> strokes; }
RACE_DETECTOR(822CE118, {
    float& stroke = strokes[detector];
    if (b.x_held_seconds <= 0) stroke = 0;
    else {
        stroke += frame_seconds();
        if (stroke >= 0.5f) { stroke = 0; set_bits(entry(results) + 20, 0x80); result = 1; }
    }
})

// The detector at 821A2484 (original 822CB9B8) uses the one at 821A2344
// (822C8958) while the pad drives the race.
PPC_FUNC_IMPL(__imp__sub_822CB9B8);
SFR_HOOK(sub_822CB9B8) {
    sfr::enter_function(ctx, "sub_822CB9B8", 0x822CB9B8);
    if (pad_racing()) sub_822C8958(ctx, base);
    else __imp__sub_822CB9B8(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_822B72E0);

// Race preparation ("On your Gear!"), state at +2408: 0 starts it, 1 waits
// for the sensor, 2..6 run the body measurements of the chosen gear (each
// step is a pose the title reads from the skeleton), 7 ends the preparation
// and 8 races. A pad has no body to measure, so its preparation ends with
// the measurements skipped.
SFR_HOOK(sub_822B72E0) {
    sfr::enter_function(ctx, "sub_822B72E0", 0x822B72E0);
    const uint32_t race = ctx.r3.u32;
    __imp__sub_822B72E0(ctx, base);
    if (!pad_racing()) return;
    const uint32_t state = memory().load<uint32_t>(race + 2408);
    if (state >= 2 && state <= 6) {
        memory().store<uint32_t>(race + 2408, 7);
        std::cerr << "NUI_RACE_PREPARATION skipped=" << state << '\n';
    }
}
