#pragma once
// Walking a Xenos shader's microcode the way XenosRecomp does: the control
// flow program, and the ALU and fetch instructions its execs run. Shared by
// vertex_palette.cpp and shader_constant_use.cpp.
#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace sfr::xenos {
inline uint32_t load_be32(std::span<const uint8_t> data, size_t offset) {
    return uint32_t(data[offset]) << 24 | uint32_t(data[offset + 1]) << 16 | uint32_t(data[offset + 2]) << 8 |
           data[offset + 3];
}

// One 48-bit control flow instruction: two share every three microcode words.
inline uint64_t control_flow(std::span<const uint32_t> code, uint32_t index) {
    const uint32_t group = (index / 2) * 3;
    if (index % 2 == 0) return uint64_t(code[group]) | uint64_t(code[group + 1] & 0xFFFF) << 32;
    return uint64_t(code[group + 1] >> 16) | uint64_t(code[group + 2]) << 16;
}

// The address an exec-shaped control flow instruction runs from, or zero.
inline uint32_t exec_address(uint64_t instruction) {
    switch (uint32_t(instruction >> 44) & 15) {
    case 1: case 2:    // Exec, ExecEnd
    case 3: case 4:    // CondExec, CondExecEnd
    case 5: case 6:    // CondExecPred, CondExecPredEnd
    case 13: case 14:  // CondExecPredClean, CondExecPredCleanEnd
        return uint32_t(instruction) & 0xFFF;
    default:
        return 0;
    }
}

// The instruction addresses the translator recompiles, in its emission order:
// the control flow program runs from word zero up to the first instruction it
// executes, and every exec names a run of instructions with two sequence bits
// each (bit 0 set = a fetch).
inline std::vector<std::pair<uint32_t, bool>> instruction_order(std::span<const uint32_t> code, uint32_t size) {
    const auto pairs = [&](uint32_t bytes) { return (std::min<uint32_t>(bytes, uint32_t(code.size() * 4))) / 12; };
    uint32_t limit = size;
    for (uint32_t group = 0; group < pairs(limit); ++group)
        for (uint32_t half = 0; half < 2; ++half) {
            const uint32_t address = exec_address(control_flow(code, group * 2 + half));
            if (address) limit = std::min(limit, address * 12);
        }
    std::vector<std::pair<uint32_t, bool>> order;  // address, is a fetch
    for (uint32_t group = 0; group < pairs(limit); ++group)
        for (uint32_t half = 0; half < 2; ++half) {
            const uint64_t instruction = control_flow(code, group * 2 + half);
            if (!exec_address(instruction)) continue;
            const uint32_t address = uint32_t(instruction) & 0xFFF, count = uint32_t(instruction >> 12) & 7;
            uint32_t sequence = uint32_t(instruction >> 16) & 0xFFF;
            for (uint32_t i = 0; i < count; ++i, sequence >>= 2)
                if ((address + i) * 3 + 2 < code.size()) order.emplace_back(address + i, (sequence & 1) != 0);
        }
    return order;
}
}
