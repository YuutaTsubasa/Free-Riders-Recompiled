#pragma once
// Which of a stage's 256 float4 constants a translated shader reads, so a
// draw uploads only those (native_renderer.cpp). Every draw used to byte-swap
// both stages' whole 4 KiB register files into the upload ring; the game's
// shaders read about 41 vertex and 8 pixel registers on average, and the
// swap of the rest was ~3% of an AYN Thor race's main thread (Issue #176,
// docs/performance.md).
//
// XenosRecomp reads a float4 constant only through the name the shader's
// constant table gives it (c0..c255 of the stage, packoffset or byte offset
// registerIndex * 16); registers outside the table are the shader's own
// literal definitions, which it declares as locals. An array indexed with
// the address register (a0/aL) may reach past its declared size, up to the
// end of the register file, so a shader that addresses constants relatively
// (33 of the game's 287 vertex shaders, none of its pixel shaders) gets
// everything.
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <vector>

namespace sfr {
// float4 registers [first, first + count) of the stage.
struct ConstantRun {
    uint16_t first = 0, count = 0;
    friend bool operator==(const ConstantRun&, const ConstantRun&) = default;
};

// The registers a shader container's translation can read, ascending, with
// runs less than merge_gap registers apart joined (copying a few unused
// registers costs less than starting another copy). Nothing when that cannot
// be told (relative constant addressing, or a table or microcode this does
// not understand): the caller then uploads all of them.
inline constexpr uint16_t constant_run_merge_gap = 4;
std::optional<std::vector<ConstantRun>> shader_constant_runs(std::span<const uint8_t> container);

// SFR_CONSTANT_RUNS=0 turns this off (and the skinning palette's swap
// straight into the ring with it): every draw then uploads all 256
// registers of both stages, as before.
inline bool constant_runs_enabled() {
    static const bool enabled = [] {
        const char* setting = std::getenv("SFR_CONSTANT_RUNS");
        return !(setting && *setting == '0');
    }();
    return enabled;
}
}
