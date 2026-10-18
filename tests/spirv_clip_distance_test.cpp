#include "spirv_clip_distance.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Module {
    std::vector<uint32_t> words;
    explicit Module(uint32_t version) : words{0x07230203u, version, 0, 100, 0} {}
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

std::vector<std::vector<uint32_t>> parse(const std::vector<uint8_t>& bytes, uint32_t* bound = nullptr) {
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    if (bound) *bound = words[3];
    std::vector<std::vector<uint32_t>> instructions;
    for (size_t i = 5; i < words.size();) {
        const uint32_t count = words[i] >> 16;
        require(count && i + count <= words.size(), "malformed output");
        instructions.emplace_back(words.begin() + i, words.begin() + i + count);
        i += count;
    }
    return instructions;
}

// The shape XenosRecomp's vertex shaders have: float gl_ClipDistance[1] as
// an output, written through an access chain at the end of the function.
//   10 float, 11 uint, 12 uint 1, 13 float[1], 14 Output float[1]*,
//   15 Output float*, 16 v4float, 17 Output v4float*, 3 clip distance,
//   20 position, 30 main, 31 void fn type, 32 void, 40 uint 0, 41 1.0
Module vertex_shader(uint32_t version) {
    Module m(version);
    m.op(17, {1});   // Shader
    m.op(17, {32});  // ClipDistance
    m.op(14, {0, 1});
    m.op(15, {0, 30, 0x6e69616d, 0, 3, 20});  // Vertex "main" %3 %20
    m.op(71, {3, 11, 3});   // BuiltIn ClipDistance
    m.op(71, {20, 11, 0});  // BuiltIn Position
    m.op(19, {32});
    m.op(33, {31, 32});
    m.op(22, {10, 32});
    m.op(21, {11, 32, 0});
    m.op(43, {11, 12, 1});
    m.op(43, {11, 40, 0});
    m.op(43, {10, 41, 0x3f800000});
    m.op(28, {13, 10, 12});
    m.op(32, {14, 3, 13});
    m.op(32, {15, 3, 10});
    m.op(23, {16, 10, 4});
    m.op(32, {17, 3, 16});
    m.op(59, {14, 3, 3});
    m.op(59, {17, 20, 3});
    m.op(54, {32, 30, 0, 31});
    m.op(248, {50});
    m.op(65, {15, 51, 3, 40});
    m.op(62, {51, 41});
    m.op(253, {});
    m.op(56, {});
    return m;
}

void test_vertex_shader(uint32_t version) {
    uint32_t bound = 0;
    std::string why;
    const auto result = sfr::without_clip_distance_spirv(vertex_shader(version).bytes(), &why);
    require(result.has_value(), "refused: " + why);
    const auto out = parse(*result, &bound);
    uint32_t array_pointer = 0, float_pointer = 0;
    for (const auto& in : out) {
        const uint32_t opcode = in[0] & 0xffff;
        require(!(opcode == 17 && in[1] == 32), "the ClipDistance capability is still there");
        require(!(opcode == 71 && in[1] == 3), "the clip distance is still decorated");
        if (opcode == 32 && in[2] == 6 && in[3] == 13) array_pointer = in[1];
        if (opcode == 32 && in[2] == 6 && in[3] == 10) float_pointer = in[1];
    }
    require(array_pointer >= 100 && float_pointer >= 100 && bound == 102, "two Private pointer types with new ids");
    bool variable = false, chain = false, position = false, entry = false;
    for (const auto& in : out) {
        const uint32_t opcode = in[0] & 0xffff;
        if (opcode == 59 && in[2] == 3) variable = in[1] == array_pointer && in[3] == 6;
        if (opcode == 59 && in[2] == 20) position = in[1] == 17 && in[3] == 3;
        if (opcode == 65 && in[2] == 51) chain = in[1] == float_pointer && in[3] == 3;
        if (opcode == 15) {
            const std::vector<uint32_t> interface(in.begin() + 4, in.end());
            const bool listed = std::find(interface.begin(), interface.end(), 3u) != interface.end();
            entry = (version >= 0x00010400u) == listed &&
                    std::find(interface.begin(), interface.end(), 20u) != interface.end();
        }
    }
    require(variable, "the clip distance is not a Private variable");
    require(chain, "the access chain does not point at Private storage");
    require(position, "the position output changed");
    require(entry, "the entry point's interface is wrong for SPIR-V version " + std::to_string(version >> 8 & 0xff));
}

void test_without_clip_distance() {
    Module m(0x00010000u);
    m.op(17, {1});
    m.op(14, {0, 1});
    const auto bytes = m.bytes();
    const auto result = sfr::without_clip_distance_spirv(bytes);
    require(result && *result == bytes, "a module without a clip distance changed");
}

void test_refusals() {
    Module read = vertex_shader(0x00010000u);
    read.words.resize(read.words.size() - 2);  // before OpReturn
    read.op(61, {13, 52, 3});
    read.op(253, {});
    read.op(56, {});
    require(!sfr::without_clip_distance_spirv(read.bytes()), "a module reading its clip distance was rewritten");
    require(!sfr::without_clip_distance_spirv(std::vector<uint8_t>(7)), "a truncated module was rewritten");
}
}

int main() {
    try {
        test_vertex_shader(0x00010000u);
        test_vertex_shader(0x00010500u);
        test_without_clip_distance();
        test_refusals();
        std::cout << "spirv clip distance tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
