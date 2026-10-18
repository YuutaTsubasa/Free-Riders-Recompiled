#include "spirv_vertex_inputs.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Module {
    std::vector<uint32_t> words{0x07230203u, 0x00010500u, 0, 100, 0};
    void op(uint32_t opcode, std::initializer_list<uint32_t> operands) {
        words.push_back(uint32_t(operands.size() + 1) << 16 | opcode);
        words.insert(words.end(), operands);
    }
    std::vector<uint8_t> bytes() const {
        std::vector<uint8_t> out(words.size() * 4);
        std::memcpy(out.data(), words.data(), out.size());
        return out;
    }
};

// The Location of each variable in a module.
std::vector<std::pair<uint32_t, uint32_t>> locations(const std::vector<uint8_t>& bytes) {
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    std::vector<std::pair<uint32_t, uint32_t>> result;
    for (size_t i = 5; i < words.size();) {
        const uint32_t count = words[i] >> 16;
        require(count && i + count <= words.size(), "malformed output");
        if ((words[i] & 0xffff) == 71 && words[i + 2] == 30) result.emplace_back(words[i + 1], words[i + 3]);
        i += count;
    }
    return result;
}

// A vertex shader as XenosRecomp's go through DXC: inputs at locations 18,
// 0, 13 and 4 (in that order of declaration), gl_VertexIndex as a builtin
// input, outputs at 0 and 16.
//   10 float, 11 v4float, 12 int, 13 Input v4float*, 14 Output v4float*,
//   15 Input int*, 2..5 inputs, 6 vertex index, 7..8 outputs
Module vertex_shader(uint32_t execution_model = 0) {
    Module m;
    m.op(17, {1});                                              // Shader
    m.op(14, {0, 1});
    m.op(15, {execution_model, 30, 0x6e69616d, 0, 2, 3, 4, 5, 6, 7, 8});  // "main" and its interface
    m.op(71, {2, 30, 18});
    m.op(71, {3, 30, 0});
    m.op(71, {4, 30, 13});
    m.op(71, {5, 30, 4});
    m.op(71, {6, 11, 42});                                      // BuiltIn VertexIndex
    m.op(71, {7, 30, 0});
    m.op(71, {8, 30, 16});
    m.op(22, {10, 32});
    m.op(23, {11, 10, 4});
    m.op(21, {12, 32, 1});
    m.op(32, {13, 1, 11});
    m.op(32, {14, 3, 11});
    m.op(32, {15, 1, 12});
    for (const uint32_t id : {2u, 3u, 4u, 5u}) m.op(59, {13, id, 1});
    m.op(59, {15, 6, 1});
    m.op(59, {14, 7, 3});
    m.op(59, {14, 8, 3});
    return m;
}

void compacts_a_vertex_shader() {
    std::string why;
    const auto compacted = sfr::compact_vertex_inputs_spirv(vertex_shader().bytes(), 16, &why);
    require(compacted.has_value(), "a vertex shader compacts: " + why);
    require(compacted->vertex, "it is a vertex shader");
    require(compacted->locations == std::vector<uint32_t>{0, 4, 13, 18}, "the inputs' locations, in order");
    const auto after = locations(compacted->code);
    const std::vector<std::pair<uint32_t, uint32_t>> expected{{2, 3}, {3, 0}, {4, 2}, {5, 1}, {7, 0}, {8, 16}};
    require(after == expected, "inputs take 0 to 3 by their old order; outputs keep theirs");
    require(compacted->code.size() == vertex_shader().bytes().size(), "nothing else changes in size");
}

void leaves_a_pixel_shader() {
    const auto module = vertex_shader(4);  // Fragment
    const auto compacted = sfr::compact_vertex_inputs_spirv(module.bytes(), 16);
    require(compacted.has_value() && !compacted->vertex, "a pixel shader is taken");
    require(compacted->code == module.bytes() && compacted->locations.empty(), "and left as it is");
}

void refuses_what_it_cannot_do() {
    std::string why;
    require(!sfr::compact_vertex_inputs_spirv(vertex_shader().bytes(), 3, &why), "more inputs than attributes");
    require(why.find("more than the 3") != std::string::npos, "says why: " + why);
    auto twice = vertex_shader();
    // %3's location: after the header (5), Capability (2), MemoryModel (3),
    // EntryPoint (12), %2's Location (4) and the rest of its own (3).
    require(twice.words[5 + 2 + 3 + 12 + 4 + 3] == 0, "the word is %3's location");
    twice.words[5 + 2 + 3 + 12 + 4 + 3] = 18;
    require(!sfr::compact_vertex_inputs_spirv(twice.bytes(), 16, &why), "two inputs at one location: " + why);
    auto array = vertex_shader();
    array.op(28, {40, 11, 41});  // an array input (two locations)
    array.op(32, {42, 1, 40});
    array.op(59, {42, 43, 1});
    array.op(71, {43, 30, 7});
    require(!sfr::compact_vertex_inputs_spirv(array.bytes(), 16, &why), "an input of two locations: " + why);
    require(!sfr::compact_vertex_inputs_spirv(std::vector<uint8_t>(12), 16), "not a module");
}

struct Element { uint32_t location, offset; };

void compacts_the_elements() {
    std::vector<Element> elements;
    for (uint32_t location = 0; location < 20; ++location) elements.push_back({location, location * 4});
    const std::vector<uint32_t> inputs{0, 4, 13, 18};
    const auto compacted = sfr::compact_vertex_elements(std::span<const Element>(elements), std::span<const uint32_t>(inputs));
    require(compacted.size() == 4, "one element an input");
    for (uint32_t rank = 0; rank < 4; ++rank)
        require(compacted[rank].location == rank && compacted[rank].offset == inputs[rank] * 4,
                "each input's element at its new location");
    const std::vector<uint32_t> none;
    require(sfr::compact_vertex_elements(std::span<const Element>(elements), std::span<const uint32_t>(none)).empty(),
            "a shader without inputs declares none");
}

// SFR_SHADER_PACK=<shaders.pack>: every vertex shader of a real pack fits
// sixteen attributes (none reads more than fourteen).
void compacts_a_shader_pack() {
    const char* path = std::getenv("SFR_SHADER_PACK");
    if (!path || !*path) return;
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    require(data.size() >= 16, "the pack is readable");
    uint32_t count;
    std::memcpy(&count, data.data() + 12, 4);
    size_t at = 16, vertex = 0, widest = 0, beyond = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t header[5];
        require(at + 20 <= data.size(), "the pack is whole");
        std::memcpy(header, data.data() + at, 20);
        at += 20;
        const size_t spirv_at = at + header[2] + header[3];
        at = spirv_at + header[4];
        require(at <= data.size(), "the pack is whole");
        if (!header[4]) continue;
        std::string why;
        const auto compacted = sfr::compact_vertex_inputs_spirv({data.data() + spirv_at, header[4]}, 16, &why);
        require(compacted.has_value(), "pack shader " + std::to_string(i) + " compacts: " + why);
        if (!compacted->vertex) continue;
        ++vertex;
        widest = (std::max)(widest, compacted->locations.size());
        if (!compacted->locations.empty() && compacted->locations.back() >= 16) ++beyond;
    }
    std::cout << "pack vertex shaders=" << vertex << " most inputs=" << widest << " reading a location past 15=" << beyond
              << '\n';
}
}  // namespace

int main() {
    try {
        compacts_a_vertex_shader();
        leaves_a_pixel_shader();
        refuses_what_it_cannot_do();
        compacts_the_elements();
        compacts_a_shader_pack();
    } catch (const std::exception& error) {
        std::cerr << "spirv vertex inputs test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "spirv vertex inputs tests passed\n";
    return 0;
}
