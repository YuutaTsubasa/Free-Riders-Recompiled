#pragma once
#include <array>
#include <cstdint>
#include <ostream>

namespace sfr {
// Diagnostic evidence, not a gameplay-state classifier. 82246818 constructs
// the race object at [83E52F8C] before loading finishes. 82230F38 reads its
// controller at +80; 82247528 dispatches that controller's update. The raw
// +12 state and constructor configuration still need semantic verification.
enum class RaceProbeStatus { absent, complete, unreadable, changed };
struct RaceProgressProbe {
    RaceProbeStatus status = RaceProbeStatus::unreadable;
    uint32_t object = 0, vtable = 0, controller = 0, controller_vtable = 0, state = 0;
    // 82247528 gates readiness update on +20. The embedded +88 readiness
    // aggregate (8227DF90) owns state +8 and resets +12/+16/+28/+32; their
    // units/clock meaning are unverified. Constructor configuration follows.
    // Keep byte offsets in the schema rather than inventing time/course names.
    static constexpr std::array<uint32_t, 13> offsets{
        20, 96, 100, 104, 116, 120, 128, 132, 136, 140, 148, 152, 156};
    std::array<uint32_t, offsets.size()> config{};
};

template<class Memory>
RaceProgressProbe read_race_progress_probe(const Memory& memory) {
    constexpr uint32_t global = 0x83E52F8C;
    RaceProgressProbe sample;
    if (!memory.readable(global, 4)) return sample;
    sample.object = memory.template load<uint32_t>(global);
    if (!sample.object) {
        sample.status = RaceProbeStatus::absent;
        return sample;
    }
    // Widen before arithmetic so corrupt/wrapping pointers cannot select a
    // low address. Only read the small, known constructor-owned member span.
    if ((sample.object & 3) || uint64_t(sample.object) + 160 > 0x100000000ull ||
        !memory.readable(sample.object, 160)) return sample;
    sample.vtable = memory.template load<uint32_t>(sample.object);
    sample.controller = memory.template load<uint32_t>(uint64_t(sample.object) + 80);
    if (!sample.controller || (sample.controller & 3) ||
        uint64_t(sample.controller) + 16 > 0x100000000ull ||
        !memory.readable(sample.controller, 16)) return sample;
    sample.controller_vtable = memory.template load<uint32_t>(sample.controller);
    sample.state = memory.template load<uint32_t>(uint64_t(sample.controller) + 12);
    for (unsigned i = 0; i < sample.offsets.size(); ++i)
        sample.config[i] = memory.template load<uint32_t>(uint64_t(sample.object) + sample.offsets[i]);
    // Reject an observed ownership change. This is not an atomic snapshot or
    // proof against same-address reuse, and must never drive game behaviour.
    sample.status = memory.template load<uint32_t>(global) == sample.object &&
        memory.template load<uint32_t>(uint64_t(sample.object) + 80) == sample.controller &&
        memory.template load<uint32_t>(sample.object) == sample.vtable &&
        memory.template load<uint32_t>(sample.controller) == sample.controller_vtable ?
        RaceProbeStatus::complete : RaceProbeStatus::changed;
    return sample;
}

inline void write_race_progress_probe(std::ostream& out, const RaceProgressProbe& sample) {
    const char* status = "unreadable";
    switch (sample.status) {
    case RaceProbeStatus::absent: status = "absent"; break;
    case RaceProbeStatus::complete: status = "complete"; break;
    case RaceProbeStatus::changed: status = "changed"; break;
    case RaceProbeStatus::unreadable: break;
    }
    out << " race_probe=1 race_sample=" << status << " race_object=" << sample.object;
    if (sample.status != RaceProbeStatus::complete) return;
    out << " race_vtable=" << sample.vtable << " race_controller=" << sample.controller
        << " race_controller_vtable=" << sample.controller_vtable << " race_state_raw=" << sample.state;
    for (unsigned i = 0; i < sample.offsets.size(); ++i)
        out << " race_raw_" << sample.offsets[i] << '=' << sample.config[i];
}
}
