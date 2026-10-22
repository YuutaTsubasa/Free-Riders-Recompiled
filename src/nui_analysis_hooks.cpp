#include "ppc_recomp_shared.h"
#include "diagnostic_hooks.h"
#include "guest_memory.h"
#include "kinect_nui_record.h"
#include "nui_analysis_trace.h"

#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <map>
#include <sstream>
#include <utility>

// Observation of the title's own use of its skeletons (docs/kinect-sensor.md,
// "觀察用 trace"), with SFR_NUI_ANALYSIS_TRACE=1 only. Each hook runs the
// original as it is and afterwards only reads guest memory.
//
// - NUI_TRACE_CALIB: the race preparation's ("On your Gear!") measuring
//   detectors write a player's base values into the calibration object at
//   [source + 92] and answer 1 once they have: 822C65B0 (arms forward: +132
//   the hip centre's z, +112/+116 the depth centroid's column and row, +120
//   the outline's height) and 822C6AD8 (the body: +0..+76 lengths and
//   directions, +124/+128 head and hips above the floor, +132 the hips' z).
//   Each time one of them answers 1 for a source after not doing so, the
//   whole object +0..+132 is written once. With SFR_KINECT_RECORD and a
//   real Kinect it also goes into the player's next row of <prefix>-nui.csv
//   (kinect_nui_record.h), trace or not.
// - NUI_TRACE_BIND: the binding of players to body records (82222B80, every
//   frame from 82222908): its mode at box +176, the two players' records at
//   box +120/+124 and their enrollments at box +136/+140 (box = [83E52F88]),
//   with the candidates it counted (+180/+184) and the team mode (83E515E7: 3
//   is a relay); a line whenever one of them changes.

namespace {
constexpr uint32_t nui_box = 0x83E52F88, team_mode = 0x83E515E7, body_reader = 0x82918418;

sfr::GuestMemory& memory() { return *sfr::active_memory; }
uint32_t word(uint64_t address) { return memory().readable(address, 4) ? memory().load<uint32_t>(address) : 0u; }
float number(uint64_t address) { return std::bit_cast<float>(word(address)); }

// The body record a race source reads (its object's vtable[1]), when that
// is the title's plain reader (lwz r3,4(r3)): no guest call is made.
uint32_t record_of_source(uint32_t source) {
    const uint32_t object = source ? word(source) : 0;
    const uint32_t vtable = object ? word(object) : 0;
    return vtable && word(uint64_t(vtable) + 4) == body_reader ? word(uint64_t(object) + 4) : 0;
}
uint32_t tracking_id_of_record(uint32_t record) {
    const uint32_t data = record ? word(uint64_t(record) + 768) : 0;
    return data ? word(uint64_t(data) + 4) : 0;
}

// The player (0, 1) whose body record this is: the binding's (box
// +120/+124), or the input's (its block +12); -1 for none.
int player_of_record(uint32_t record) {
    if (!record) return -1;
    if (const uint32_t box = word(nui_box))
        for (uint32_t p = 0; p < 2; ++p)
            if (word(uint64_t(box) + 120 + 4 * p) == record) return int(p);
    if (const uint32_t input = word(0x83E52FB8))
        for (uint32_t p = 0; p < 2; ++p)
            if (word(uint64_t(input) + 32 + 2544 * p + 12) == record) return int(p);
    return -1;
}

void calibration_completed(const char* step, uint32_t address, uint32_t source, uint32_t result) {
    static std::map<std::pair<uint32_t, uint32_t>, uint32_t> last;
    uint32_t& before = last[{address, source}];
    const bool completed = result == 1 && before != 1;
    before = result;
    if (!completed) return;
    const uint32_t calib = word(uint64_t(source) + 92);
    const uint32_t record = record_of_source(source);
    // SFR_KINECT_RECORD: into the player's next nui.csv row
    // (kinect_nui_record.h), for a real Kinect's player.
    if (calib && memory().readable(calib, 136) && sfr::nui_body_from_sensor() && sfr::kinect_record_nui()) {
        std::array<uint32_t, sfr::kinect_nui_calibration_words> words{};
        for (uint32_t i = 0; i < words.size(); ++i) words[i] = word(uint64_t(calib) + 4 * i);
        if (const int player = player_of_record(record); player >= 0)
            sfr::kinect_nui_recorder().calibration(uint32_t(player), step, words);
    }
    static sfr::NuiTraceLimiter limiter("CALIB", 20.0, 2000);
    if (!limiter.take()) return;
    std::ostringstream line;
    line << "NUI_TRACE_CALIB step=" << step << " function=0x" << std::hex << address << " source=0x" << source
         << " calib=0x" << calib << " record=0x" << record << std::dec << " tracking_id=" << tracking_id_of_record(record)
         << " slot=" << (record ? int(word(uint64_t(record) + 784)) : -1);
    if (calib && memory().readable(calib, 136)) {
        for (uint32_t offset = 0; offset <= 132; offset += 4) {
            line << " +" << offset << '=';
            // +112/+116/+120 are whole numbers (the depth centroid and the
            // outline's height, in pixels); the rest floats.
            if (offset >= 112 && offset <= 120) line << int32_t(word(uint64_t(calib) + offset));
            else line << number(uint64_t(calib) + offset);
        }
    }
    std::cerr << line.str() << '\n';
}
}

PPC_FUNC_IMPL(__imp__sub_822C65B0);
SFR_INPUT_HOOK(sub_822C65B0) {
    sfr::enter_function(ctx, "sub_822C65B0", 0x822C65B0);
    const uint32_t source = ctx.r4.u32;
    __imp__sub_822C65B0(ctx, base);
    calibration_completed("arms_forward", 0x822C65B0, source, ctx.r3.u32);
}

PPC_FUNC_IMPL(__imp__sub_822C6AD8);
SFR_INPUT_HOOK(sub_822C6AD8) {
    sfr::enter_function(ctx, "sub_822C6AD8", 0x822C6AD8);
    const uint32_t source = ctx.r4.u32;
    __imp__sub_822C6AD8(ctx, base);
    calibration_completed("body", 0x822C6AD8, source, ctx.r3.u32);
}

PPC_FUNC_IMPL(__imp__sub_82222B80);
SFR_INPUT_HOOK(sub_82222B80) {
    sfr::enter_function(ctx, "sub_82222B80", 0x82222B80);
    __imp__sub_82222B80(ctx, base);
    if (!sfr::nui_analysis_trace()) return;
    const uint32_t box = word(nui_box);
    if (!box || !memory().readable(box, 188)) return;
    const std::array<uint32_t, 7> now{word(uint64_t(box) + 176), word(uint64_t(box) + 120), word(uint64_t(box) + 124),
                                      word(uint64_t(box) + 136), word(uint64_t(box) + 140), word(uint64_t(box) + 180),
                                      uint32_t(memory().readable(team_mode, 1) ? memory().load<uint8_t>(team_mode) : 0xFF)};
    static std::array<uint32_t, 7> before{};
    static bool seen = false;
    // The candidate count (+180) alone changes whenever someone steps in or
    // out; only the binding itself makes a line.
    const bool changed = !seen || now[0] != before[0] || now[1] != before[1] || now[2] != before[2] ||
                         now[3] != before[3] || now[4] != before[4] || now[6] != before[6];
    if (!changed) return;
    static sfr::NuiTraceLimiter limiter("BIND", 10.0, 2000);
    if (!limiter.take()) return;  // tried again next frame: the change is still there
    seen = true;
    before = now;
    std::ostringstream line;
    line << "NUI_TRACE_BIND mode=" << int32_t(now[0]) << " team_mode=" << now[6] << " box=0x" << std::hex << box
         << " record0=0x" << now[1] << " record1=0x" << now[2] << std::dec << " tracking_id0=" << tracking_id_of_record(now[1])
         << " tracking_id1=" << tracking_id_of_record(now[2]) << " enrollment0=" << int32_t(now[3])
         << " enrollment1=" << int32_t(now[4]) << " candidates=" << now[5] << " candidates_new=" << word(uint64_t(box) + 184);
    std::cerr << line.str() << '\n';
}
