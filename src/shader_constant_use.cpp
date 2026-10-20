#include "shader_constant_use.h"
#include "xenos_microcode.h"
#include <bitset>

namespace sfr {
namespace {
using xenos::load_be32;

uint16_t load_be16(std::span<const uint8_t> data, size_t offset) {
    return uint16_t(data[offset] << 8 | data[offset + 1]);
}

constexpr uint32_t register_file = 256;  // float4 constants per stage
constexpr uint16_t float4_set = 2;       // D3DXRS_FLOAT4
constexpr size_t constant_info_size = 20;  // D3DXSHADER_CONSTANTINFO
}

std::optional<std::vector<ConstantRun>> shader_constant_runs(std::span<const uint8_t> container) {
    const auto readable = [&](size_t offset, size_t bytes) {
        return offset <= container.size() && bytes <= container.size() - offset;
    };
    if (!readable(0, 0x24)) return std::nullopt;

    // The constant table (ShaderContainer +0x10): a size word, then
    // D3DXSHADER_CONSTANTTABLE, whose offsets count from its own start.
    const size_t table = size_t(load_be32(container, 0x10)) + 4;
    if (!load_be32(container, 0x10) || !readable(table, 24)) return std::nullopt;
    const uint32_t constants = load_be32(container, table + 12);
    const size_t info = table + load_be32(container, table + 16);
    if (constants > 4096 || !readable(info, size_t(constants) * constant_info_size)) return std::nullopt;
    std::bitset<register_file> used;
    for (uint32_t i = 0; i < constants; ++i) {
        const size_t at = info + size_t(i) * constant_info_size;
        if (load_be16(container, at + 4) != float4_set) continue;
        const uint32_t first = load_be16(container, at + 6), count = load_be16(container, at + 8);
        if (first + count > register_file) return std::nullopt;
        for (uint32_t r = first; r < first + count; ++r) used.set(r);
    }

    // The microcode (Shader at ShaderContainer +0x18: its physical offset past
    // the container's virtual part, and its size). An ALU instruction whose
    // constant operands are relative (const0Relative bit 31 / const1Relative
    // bit 30 of its second word) may read any register past its array.
    const size_t shader = load_be32(container, 0x18);
    if (!readable(shader, 8)) return std::nullopt;
    const size_t code_at = size_t(load_be32(container, 4)) + load_be32(container, shader);
    const uint32_t size = load_be32(container, shader + 4);
    if (code_at % 4 || code_at >= container.size()) return std::nullopt;
    std::vector<uint32_t> code((container.size() - code_at) / 4);
    for (size_t i = 0; i < code.size(); ++i) code[i] = load_be32(container, code_at + i * 4);
    if (code.size() < 3) return std::nullopt;
    for (const auto [address, is_fetch] : xenos::instruction_order(code, size))
        if (!is_fetch && (code[address * 3 + 1] >> 30)) return std::nullopt;

    std::vector<ConstantRun> runs;
    for (uint32_t r = 0; r < register_file;) {
        if (!used.test(r)) { ++r; continue; }
        uint32_t end = r;
        while (end < register_file && used.test(end)) ++end;
        if (!runs.empty() && r - (runs.back().first + runs.back().count) < constant_run_merge_gap)
            runs.back().count = uint16_t(end - runs.back().first);
        else
            runs.push_back({uint16_t(r), uint16_t(end - r)});
        r = end;
    }
    return runs;
}
}
