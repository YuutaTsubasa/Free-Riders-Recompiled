#include "shader_constant_use.h"
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

void put_be32(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
    if (data.size() < offset + 4) data.resize(offset + 4);
    for (int b = 0; b < 4; ++b) data[offset + b] = uint8_t(value >> (24 - 8 * b));
}

void put_be16(std::vector<uint8_t>& data, size_t offset, uint16_t value) {
    if (data.size() < offset + 2) data.resize(offset + 2);
    data[offset] = uint8_t(value >> 8);
    data[offset + 1] = uint8_t(value);
}

struct Constant { uint16_t set, first, count; };

// A container with a constant table of these entries and one exec of a
// single ALU instruction whose second word is alu_word1.
constexpr uint32_t table_at = 0x40, shader_at = 0x24, virtual_size = 0x200, code_at = virtual_size;

std::vector<uint8_t> container(const std::vector<Constant>& constants, uint32_t alu_word1 = 0) {
    std::vector<uint8_t> data(virtual_size, 0);
    put_be32(data, 0, 1);  // flags: a vertex shader
    put_be32(data, 4, virtual_size);
    put_be32(data, 0x10, table_at);
    put_be32(data, 0x18, shader_at);
    put_be32(data, shader_at + 0, 0);   // physical offset
    put_be32(data, shader_at + 4, 24);  // microcode size in bytes
    // ConstantTableContainer: size, then D3DXSHADER_CONSTANTTABLE (offsets
    // from its start): constants at +12, their info at +16.
    const size_t table = table_at + 4;
    put_be32(data, table + 12, uint32_t(constants.size()));
    put_be32(data, table + 16, 28);
    for (size_t i = 0; i < constants.size(); ++i) {
        const size_t at = table + 28 + i * 20;
        put_be16(data, at + 4, constants[i].set);
        put_be16(data, at + 6, constants[i].first);
        put_be16(data, at + 8, constants[i].count);
    }
    // Exec from address 1, one ALU instruction (sequence bits 0).
    put_be32(data, code_at + 0, 1 | 1 << 12);
    put_be32(data, code_at + 4, 1 << 12);  // opcode 1 (Exec) in bits 44..47
    put_be32(data, code_at + 8, 0);
    put_be32(data, code_at + 12, 0);
    put_be32(data, code_at + 16, alu_word1);
    put_be32(data, code_at + 20, 0);
    return data;
}

using Runs = std::vector<sfr::ConstantRun>;
}

int main() {
    try {
        // Float4 entries only (samplers, bools and ints live elsewhere), in
        // register order, with near runs joined.
        auto runs = sfr::shader_constant_runs(container({{2, 8, 4}, {3, 0, 1}, {2, 0, 2}, {0, 30, 1}, {2, 13, 1}}));
        require(runs && *runs == Runs{{0, 2}, {8, 6}}, "float4 entries in register order, near runs joined");
        runs = sfr::shader_constant_runs(container({{2, 0, 1}, {2, 200, 24}}));
        require(runs && *runs == Runs{{0, 1}, {200, 24}}, "distant runs stay apart");
        runs = sfr::shader_constant_runs(container({}));
        require(runs && runs->empty(), "a shader without float4 constants reads none");
        // Relative addressing (const0Relative bit 31, const1Relative bit 30)
        // may reach any later register: unknown.
        require(!sfr::shader_constant_runs(container({{2, 8, 4}}, 1u << 31)), "relative constant 0");
        require(!sfr::shader_constant_runs(container({{2, 8, 4}}, 1u << 30)), "relative constant 1");
        require(sfr::shader_constant_runs(container({{2, 8, 4}}, 1u << 29)).has_value(), "other bits are not relative");
        // Out of the register file or the container: unknown.
        require(!sfr::shader_constant_runs(container({{2, 250, 8}})), "past the register file");
        auto truncated = container({{2, 0, 1}});
        truncated.resize(0x50);
        require(!sfr::shader_constant_runs(truncated), "truncated container");
        require(!sfr::shader_constant_runs(std::vector<uint8_t>(8, 0)), "too small for a header");
        std::cout << "shader constant use tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
