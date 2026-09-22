#include "vertex_palette.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>

namespace sfr {
namespace {
// The declaration usages XenosRecomp maps to a Vulkan location, in the order
// the patch prefers them. Its Metal path exits on a usage outside that table,
// and these four read straight through (the rest are wrapped in swapFloats).
constexpr uint32_t candidates[][2] = {{0, 3}, {0, 2}, {0, 1}, {10, 0}};
// Vertex fetch constant 94: stream 1, as SetStreamSource fills it.
constexpr uint32_t palette_constant = 94;
constexpr uint32_t float4_format = 38;  // FMT_32_32_32_32_FLOAT
constexpr uint32_t palette_capacity = 1024;  // float4 in the constant buffer

constexpr const char* usage_variables[] = {"Position", "BlendWeight", "BlendIndices", "Normal", "PointSize",
                                           "TexCoord", "Tangent", "Binormal", "TessFactor", "PositionT",
                                           "Color", "Fog", "Depth", "Sample"};

bool readable(std::span<const uint8_t> data, size_t offset) { return offset + 4 <= data.size(); }

uint32_t load_be32(std::span<const uint8_t> data, size_t offset) {
    return uint32_t(data[offset]) << 24 | uint32_t(data[offset + 1]) << 16 | uint32_t(data[offset + 2]) << 8 |
           data[offset + 3];
}

void store_be32(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
    for (int b = 0; b < 4; ++b) data[offset + b] = uint8_t(value >> (24 - 8 * b));
}

// One 48-bit control flow instruction: two share every three microcode words.
uint64_t control_flow(std::span<const uint32_t> code, uint32_t index) {
    const uint32_t group = (index / 2) * 3;
    if (index % 2 == 0) return uint64_t(code[group]) | uint64_t(code[group + 1] & 0xFFFF) << 32;
    return uint64_t(code[group + 1] >> 16) | uint64_t(code[group + 2]) << 16;
}

// The address an exec-shaped control flow instruction runs from, or zero.
uint32_t exec_address(uint64_t instruction) {
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
std::vector<std::pair<uint32_t, bool>> instruction_order(std::span<const uint32_t> code, uint32_t size) {
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

std::optional<VertexPalette> vertex_palette(std::span<const uint8_t> container) {
    if (container.size() < 0x24 || (load_be32(container, 0) & 1) == 0) return std::nullopt;  // pixel shader
    const size_t shader = load_be32(container, 0x18);
    if (!readable(container, shader + 0x20)) return std::nullopt;
    const uint32_t virtual_size = load_be32(container, 4), physical_offset = load_be32(container, shader);
    const uint32_t size = load_be32(container, shader + 4), interpolator_info = load_be32(container, shader + 0x14);
    const uint32_t prefix = load_be32(container, shader + 0x18), count = load_be32(container, shader + 0x1C);
    const uint32_t interpolators = (interpolator_info >> 5) & 0x1F;
    if (count > 64 || prefix > 64 || shader + 0x24 + size_t(prefix + count + interpolators) * 4 > container.size())
        return std::nullopt;
    const size_t code_at = size_t(virtual_size) + physical_offset;
    if (code_at % 4 || code_at >= container.size()) return std::nullopt;
    std::vector<uint32_t> code((container.size() - code_at) / 4);
    for (size_t i = 0; i < code.size(); ++i) code[i] = load_be32(container, code_at + i * 4);
    if (code.size() < 3) return std::nullopt;

    std::vector<uint32_t> declared;
    for (uint32_t i = 0; i < count; ++i)
        declared.push_back(load_be32(container, shader + 0x24 + size_t(prefix + i) * 4) & 0xFFF);

    VertexPalette palette;
    // A mini fetch carries only a destination and an offset: the constant, the
    // format, the entry stride and the index it reads come from the full fetch
    // before it.
    uint32_t inherited_register = 0, inherited_component = 0, inherited_stride = 0;
    uint32_t inherited_constant = 0, inherited_format = 0;
    for (const auto [address, is_fetch] : instruction_order(code, size)) {
        if (!is_fetch) continue;
        const uint32_t word0 = code[address * 3], word1 = code[address * 3 + 1], word2 = code[address * 3 + 2];
        if (word0 & 31) continue;  // a texture fetch
        const bool mini = (word1 >> 30) & 1;
        if (!mini) {
            inherited_register = (word0 >> 5) & 63;
            inherited_component = (word0 >> 30) & 3;
            inherited_stride = word2 & 0xFF;
            inherited_constant = ((word0 >> 20) & 31) * 3 + ((word0 >> 25) & 3);
            inherited_format = (word1 >> 16) & 63;
        }
        if (std::find(declared.begin(), declared.end(), address) != declared.end()) continue;
        // Only the skinning palette is understood: a float4 row of a
        // fixed-size entry in the buffer stream 1 provides.
        const uint32_t offset = (word2 >> 8) & 0x7FFFFF;
        if (inherited_constant != palette_constant || inherited_format != float4_format) return std::nullopt;
        if (!inherited_stride || inherited_stride % 4 || offset % 4 || offset >= inherited_stride) return std::nullopt;
        if (palette.entry_float4s && palette.entry_float4s != inherited_stride / 4) return std::nullopt;
        palette.entry_float4s = inherited_stride / 4;
        palette.fetches.push_back({address, inherited_register, inherited_component, offset / 4});
    }
    if (palette.fetches.empty()) return std::nullopt;

    const auto free_usage = [&](uint32_t usage, uint32_t index) {
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t value = load_be32(container, shader + 0x24 + size_t(prefix + i) * 4);
            if (((value >> 12) & 15) == usage && ((value >> 16) & 15) == index) return false;
        }
        return true;
    };
    const auto chosen = std::find_if(std::begin(candidates), std::end(candidates),
                                     [&](const auto& pair) { return free_usage(pair[0], pair[1]); });
    if (chosen == std::end(candidates)) return std::nullopt;
    palette.usage = (*chosen)[0];
    palette.usage_index = (*chosen)[1];

    // The patched shader is appended whole, with its declaration enlarged, and
    // the container's shader offset repointed at it. Nothing else refers to it,
    // and the microcode keeps its place.
    palette.container.assign(container.begin(), container.end());
    const size_t patched = palette.container.size();
    const uint32_t added = uint32_t(palette.fetches.size());
    palette.container.resize(patched + 0x24 + size_t(prefix + count + added + interpolators) * 4);
    std::memcpy(palette.container.data() + patched, container.data() + shader, 0x24);
    store_be32(palette.container, patched + 0x1C, count + added);
    const auto word = [&](uint32_t i) { return load_be32(container, shader + 0x24 + size_t(i) * 4); };
    size_t at = patched + 0x24;
    for (uint32_t i = 0; i < prefix + count; ++i, at += 4) store_be32(palette.container, at, word(i));
    for (const auto& fetch : palette.fetches) {
        store_be32(palette.container, at, fetch.address | palette.usage << 12 | palette.usage_index << 16);
        at += 4;
    }
    for (uint32_t i = 0; i < interpolators; ++i, at += 4) store_be32(palette.container, at, word(prefix + count + i));
    store_be32(palette.container, 0x18, uint32_t(patched));
    return palette;
}

std::optional<std::string> apply_vertex_palette(const VertexPalette& palette, const std::string& hlsl) {
    if (palette.usage >= std::size(usage_variables)) return std::nullopt;
    const std::string name = std::string("i") + usage_variables[palette.usage] + std::to_string(palette.usage_index);
    const auto token = [&](const std::string& text, size_t at) {
        const size_t after = at + name.size();
        return after >= text.size() || !(std::isalnum(static_cast<unsigned char>(text[after])) || text[after] == '_');
    };

    // The declaration the patch added is unused: every read becomes a palette
    // load, and the input itself goes away so no vertex buffer has to feed it.
    const std::string read = "input." + name;
    std::string text;
    for (size_t line_at = 0; line_at < hlsl.size();) {
        const size_t newline = hlsl.find('\n', line_at);
        const size_t end = newline == std::string::npos ? hlsl.size() : newline + 1;
        const std::string line = hlsl.substr(line_at, end - line_at);
        const size_t found = line.find(name);
        const bool declaration =
            found != std::string::npos && token(line, found) && line.find(read) == std::string::npos;
        if (!declaration) text += line;
        line_at = end;
    }

    std::string result;
    size_t at = 0, replaced = 0;
    for (size_t found = text.find(read); found != std::string::npos; found = text.find(read, at)) {
        result += text.substr(at, found - at);
        at = found + read.size();
        if (!token(text, found + 6)) {  // a longer name with the same prefix
            result += read;
            continue;
        }
        if (replaced >= palette.fetches.size()) return std::nullopt;
        const auto& fetch = palette.fetches[replaced++];
        result += "sfrVertexPalette(int(r" + std::to_string(fetch.source_register) + '.' +
                  "xyzw"[fetch.source_component] + ") * " + std::to_string(palette.entry_float4s) + " + " +
                  std::to_string(fetch.row) + ")";
    }
    if (replaced != palette.fetches.size()) return std::nullopt;
    result += text.substr(at);

    const std::string anchor = "struct VertexShaderInput";
    const size_t insert = result.find(anchor);
    if (insert == std::string::npos) return std::nullopt;
    result.insert(insert, "cbuffer VertexPalette : register(b3, space4)\n{\n\tfloat4 g_VertexPalette[" +
                              std::to_string(palette_capacity) + "];\n};\n\nfloat4 sfrVertexPalette(int index)\n{\n"
                              "\treturn g_VertexPalette[uint(index) & " + std::to_string(palette_capacity - 1) +
                              "u];\n}\n\n");
    return result;
}

uint32_t vertex_palette_entries(const VertexPalette& palette, uint32_t stream_bytes) {
    if (!palette.entry_float4s) return 0;
    return std::min(stream_bytes / (palette.entry_float4s * 16), palette_capacity / palette.entry_float4s);
}
}
