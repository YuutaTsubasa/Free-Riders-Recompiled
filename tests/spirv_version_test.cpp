#include "spirv_bounded_descriptors.h"
#include "spirv_clip_distance.h"
#include "spirv_version.h"
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

std::vector<std::vector<uint32_t>> parse(const std::vector<uint8_t>& bytes, uint32_t* version = nullptr) {
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    if (version) *version = words[1];
    std::vector<std::vector<uint32_t>> instructions;
    for (size_t i = 5; i < words.size();) {
        const uint32_t count = words[i] >> 16;
        require(count && i + count <= words.size(), "malformed output");
        instructions.emplace_back(words.begin() + i, words.begin() + i + count);
        i += count;
    }
    return instructions;
}

// A pixel shader the way DXC writes SPIR-V 1.5: the interface lists its
// input, its output and the uniform buffer and texture array it reads.
//   10 float, 16 v4float, 17 Input v4float*, 18 Output v4float*,
//   19 Uniform block*, 21 UniformConstant images*, 2 input, 3 output,
//   4 block, 5 images, 30 main, 31 void fn type, 32 void, 33 select
Module pixel_shader(uint32_t version) {
    Module m(version);
    m.op(17, {1});   // Shader
    m.op(14, {0, 1});
    m.op(15, {4, 30, 0x6e69616d, 0, 2, 4, 3, 5});  // Fragment "main" %2 %4 %3 %5
    m.op(16, {30, 7});                             // OriginUpperLeft
    m.op(19, {32});
    m.op(33, {31, 32});
    m.op(22, {10, 32});
    m.op(23, {16, 10, 4});
    m.op(32, {17, 1, 16});
    m.op(32, {18, 3, 16});
    m.op(32, {19, 2, 16});
    m.op(32, {21, 0, 16});
    m.op(59, {17, 2, 1});
    m.op(59, {18, 3, 3});
    m.op(59, {19, 4, 2});
    m.op(59, {21, 5, 0});
    m.op(54, {32, 30, 0, 31});
    m.op(248, {50});
    m.op(61, {16, 51, 2});
    m.op(62, {3, 51});
    m.op(253, {});
    m.op(56, {});
    return m;
}

std::vector<uint32_t> interface_of(const std::vector<std::vector<uint32_t>>& module) {
    for (const auto& in : module)
        if ((in[0] & 0xffff) == 15) return std::vector<uint32_t>(in.begin() + 5, in.end());
    throw std::runtime_error("no entry point");
}

void lowers_a_spirv_1_5_module() {
    std::string why;
    const auto result = sfr::spirv_1_3(pixel_shader(0x00010500u).bytes(), &why);
    require(result.has_value(), "refused: " + why);
    uint32_t version = 0;
    const auto out = parse(*result, &version);
    require(version == 0x00010300u, "the version is not 1.3");
    require(interface_of(out) == std::vector<uint32_t>{2, 3}, "the interface does not keep only the input and output");
    const auto original = parse(pixel_shader(0x00010500u).bytes());
    require(out.size() == original.size(), "instructions were added or lost");
    for (size_t i = 0; i < out.size(); ++i)
        if ((out[i][0] & 0xffff) != 15) require(out[i] == original[i], "an instruction besides the entry point changed");
}

void leaves_an_older_module() {
    for (const uint32_t version : {0x00010000u, 0x00010300u}) {
        const auto bytes = pixel_shader(version).bytes();
        const auto result = sfr::spirv_1_3(bytes);
        require(result && *result == bytes, "a SPIR-V " + std::to_string(version >> 8 & 0xff) + " module changed");
    }
}

void refuses_what_spirv_1_3_lacks() {
    std::string why;
    Module memory_model(0x00010500u);
    memory_model.op(17, {5345});  // VulkanMemoryModel
    memory_model.op(14, {0, 3});
    require(!sfr::spirv_1_3(memory_model.bytes(), &why), "the Vulkan memory model was lowered");

    Module addresses(0x00010500u);
    addresses.op(17, {5347});  // PhysicalStorageBufferAddresses
    require(!sfr::spirv_1_3(addresses.bytes(), &why), "physical storage buffer addresses were lowered");

    // A select of vectors is SPIR-V 1.0; of a struct, 1.4.
    Module vectors = pixel_shader(0x00010500u);
    vectors.words.resize(vectors.words.size() - 2);  // before OpReturn
    vectors.op(169, {16, 52, 60, 51, 51});
    vectors.op(253, {});
    vectors.op(56, {});
    const bool lowered = sfr::spirv_1_3(vectors.bytes(), &why).has_value();
    require(lowered, "a select of vectors was refused: " + why);
    Module structure = pixel_shader(0x00010500u);
    structure.words.resize(structure.words.size() - 2);
    structure.op(30, {70, 10, 10});  // OpTypeStruct (out of place, but enough for the check)
    structure.op(169, {70, 52, 60, 61, 62});
    structure.op(253, {});
    structure.op(56, {});
    require(!sfr::spirv_1_3(structure.bytes(), &why), "a select of a struct was lowered");

    Module copy = pixel_shader(0x00010500u);
    copy.words.resize(copy.words.size() - 2);
    copy.op(400, {16, 53, 51});  // OpCopyLogical
    copy.op(253, {});
    copy.op(56, {});
    require(!sfr::spirv_1_3(copy.bytes(), &why), "OpCopyLogical was lowered");

    Module unknown = pixel_shader(0x00010500u);
    unknown.words[18] = 99;  // the entry point's last interface entry (%5) names no variable
    require(!sfr::spirv_1_3(unknown.bytes(), &why), "an interface entry that is no variable was accepted");
    require(!sfr::spirv_1_3(std::vector<uint8_t>(7)), "a truncated module was lowered");
}

// SFR_SHADER_PACK=<shaders.pack>: every SPIR-V shader of a real pack is
// lowered as it is, and after the other rewrites (compacted vertex inputs,
// the compact bounded layout), as an old Mali would take it. SFR_SPIRV_1_3_DUMP=<directory>
// keeps each result for
// spirv-val --target-env vulkan1.1.
void lowers_a_shader_pack() {
    const char* path = std::getenv("SFR_SHADER_PACK");
    if (!path || !*path) return;
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    require(data.size() >= 16, "the pack is readable");
    uint32_t count;
    std::memcpy(&count, data.data() + 12, 4);
    const char* dump = std::getenv("SFR_SPIRV_1_3_DUMP");
    size_t at = 16, lowered = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t header[5];
        require(at + 20 <= data.size(), "the pack is whole");
        std::memcpy(header, data.data() + at, 20);
        at += 20;
        const size_t spirv_at = at + header[2] + header[3];
        at = spirv_at + header[4];
        require(at <= data.size(), "the pack is whole");
        if (!header[4]) continue;
        const std::span<const uint8_t> code(data.data() + spirv_at, header[4]);
        std::string why;
        const auto plain = sfr::spirv_1_3(code, &why);
        require(plain.has_value(), "pack shader " + std::to_string(i) + " is lowered: " + why);
        const auto compacted = sfr::compact_vertex_inputs_spirv(code, 16, &why);
        require(compacted.has_value(), "pack shader " + std::to_string(i) + " compacts its vertex inputs: " + why);
        auto rewritten = sfr::without_clip_distance_spirv(compacted->code, &why);
        require(rewritten.has_value(), "pack shader " + std::to_string(i) + " loses its clip distance: " + why);
        rewritten = sfr::bounded_descriptor_spirv(*rewritten, &why, *sfr::bounded_lengths(16));
        require(rewritten.has_value(), "pack shader " + std::to_string(i) + " is bounded: " + why);
        const auto all = sfr::spirv_1_3(*rewritten, &why);
        require(all.has_value(), "rewritten pack shader " + std::to_string(i) + " is lowered: " + why);
        if (dump && *dump) {
            const auto base = (std::filesystem::u8path(dump) / std::to_string(i)).string();
            std::ofstream(base + ".plain.spv", std::ios::binary)
                .write(reinterpret_cast<const char*>(plain->data()), std::streamsize(plain->size()));
            std::ofstream(base + ".all.spv", std::ios::binary)
                .write(reinterpret_cast<const char*>(all->data()), std::streamsize(all->size()));
        }
        ++lowered;
    }
    std::cout << "pack shaders lowered=" << lowered << '\n';
}
}  // namespace

int main() {
    try {
        lowers_a_spirv_1_5_module();
        leaves_an_older_module();
        refuses_what_spirv_1_3_lacks();
        lowers_a_shader_pack();
        std::cout << "spirv version tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
