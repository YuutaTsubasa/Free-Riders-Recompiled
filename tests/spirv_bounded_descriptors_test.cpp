#include "spirv_bounded_descriptors.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// A small SPIR-V assembler: instructions of literal words, strings packed
// little-endian and null-terminated.
struct Module {
    std::vector<uint32_t> words{0x07230203u, 0x00010300u, 0, 100, 0};
    void op(uint32_t opcode, std::initializer_list<uint32_t> operands) {
        words.push_back(uint32_t(operands.size() + 1) << 16 | opcode);
        words.insert(words.end(), operands);
    }
    void op_string(uint32_t opcode, std::initializer_list<uint32_t> before, const std::string& text) {
        std::vector<uint32_t> packed((text.size() + 4) / 4, 0);
        std::memcpy(packed.data(), text.data(), text.size());
        words.push_back(uint32_t(before.size() + packed.size() + 1) << 16 | opcode);
        words.insert(words.end(), before);
        words.insert(words.end(), packed.begin(), packed.end());
    }
    std::vector<uint8_t> bytes() const {
        std::vector<uint8_t> out(words.size() * 4);
        std::memcpy(out.data(), words.data(), out.size());
        return out;
    }
};

struct Parsed {
    std::vector<std::vector<uint32_t>> instructions;
    uint32_t bound = 0;
    std::vector<std::vector<uint32_t>> of(uint32_t opcode) const {
        std::vector<std::vector<uint32_t>> found;
        for (const auto& in : instructions)
            if ((in[0] & 0xffff) == opcode) found.push_back(in);
        return found;
    }
    // Where the instruction defining a result id is (instructions with a
    // result id in word 1, the types).
    size_t position(uint32_t id) const {
        for (size_t i = 0; i < instructions.size(); ++i) {
            const uint32_t opcode = instructions[i][0] & 0xffff;
            if (opcode >= 19 && opcode <= 39 && instructions[i].size() > 1 && instructions[i][1] == id) return i;
            if ((opcode == 43 || opcode == 59) && instructions[i].size() > 2 && instructions[i][2] == id) return i;
        }
        return size_t(-1);
    }
    uint32_t decoration(uint32_t id, uint32_t decoration) const {
        for (const auto& in : of(71))
            if (in[1] == id && in[2] == decoration) return in[3];
        return ~0u;
    }
};

Parsed parse(const std::vector<uint8_t>& bytes) {
    require(bytes.size() % 4 == 0 && bytes.size() >= 20, "output is a module");
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    require(words[0] == 0x07230203u, "output keeps the magic");
    Parsed parsed;
    parsed.bound = words[3];
    for (size_t i = 5; i < words.size();) {
        const uint32_t count = words[i] >> 16;
        require(count && i + count <= words.size(), "output instructions are whole");
        parsed.instructions.emplace_back(words.begin() + i, words.begin() + i + count);
        i += count;
    }
    // Every result id stays under the bound.
    for (const auto& in : parsed.instructions) {
        const uint32_t opcode = in[0] & 0xffff;
        if (opcode >= 19 && opcode <= 39 && in.size() > 1) require(in[1] < parsed.bound, "type ids are under the bound");
        if ((opcode == 43 || opcode == 59) && in.size() > 2) require(in[2] < parsed.bound, "ids are under the bound");
    }
    return parsed;
}

constexpr uint32_t Shader = 1, RuntimeDescriptorArray = 5302, ShaderNonUniform = 5301, ImageQuery = 50,
                   SampledImageArrayDynamicIndexing = 29;

// The translator's shape: the three texture heaps alias set 0 binding 0 as
// runtime arrays, the samplers are set 1, the constants set 2. The 32-bit
// unsigned type comes after the arrays here, so the rewrite has to move it
// up for the length constant.
Module heaps(bool non_uniform) {
    Module m;
    m.op(17, {Shader});
    m.op(17, {RuntimeDescriptorArray});
    m.op(17, {ImageQuery});
    if (non_uniform) m.op(17, {ShaderNonUniform});
    m.op_string(10, {}, "SPV_EXT_descriptor_indexing");
    m.op(14, {0, 1});                    // OpMemoryModel Logical GLSL450
    m.op_string(15, {4, 1}, "main");     // OpEntryPoint Fragment %1 "main"
    m.op(16, {1, 7});                    // OpExecutionMode %1 OriginUpperLeft
    for (uint32_t variable : {40u, 41u, 42u}) {
        m.op(71, {variable, 34, 0});    // DescriptorSet 0
        m.op(71, {variable, 33, 0});    // Binding 0
    }
    m.op(71, {43, 34, 1});
    m.op(71, {43, 33, 0});
    m.op(71, {44, 34, 2});
    m.op(71, {44, 33, 0});
    if (non_uniform) m.op(71, {60, 5300});
    m.op(19, {2});                       // void
    m.op(33, {3, 2});                    // void()
    m.op(22, {5, 32});                   // float
    m.op(25, {10, 5, 1, 2, 0, 0, 1, 0}); // 2D
    m.op(25, {11, 5, 1, 2, 1, 0, 1, 0}); // 2D array
    m.op(25, {12, 5, 3, 2, 0, 0, 1, 0}); // cube
    m.op(26, {13});                      // sampler
    m.op(29, {20, 10});
    m.op(29, {21, 11});
    m.op(29, {22, 12});
    m.op(29, {23, 13});
    m.op(32, {30, 0, 20});
    m.op(32, {31, 0, 21});
    m.op(32, {32, 0, 22});
    m.op(32, {33, 0, 23});
    m.op(21, {6, 32, 0});                // uint, after the arrays
    m.op(30, {14, 6});                   // a storage buffer's struct { uint[] }
    m.op(29, {15, 6});
    m.op(30, {16, 15});
    m.op(32, {34, 2, 16});               // Uniform pointer
    m.op(59, {30, 40, 0});
    m.op(59, {31, 41, 0});
    m.op(59, {32, 42, 0});
    m.op(59, {33, 43, 0});
    m.op(59, {34, 44, 2});
    m.op(54, {2, 1, 0, 3});              // OpFunction
    m.op(248, {50});                     // OpLabel
    m.op(253, {});                       // OpReturn
    m.op(56, {});                        // OpFunctionEnd
    return m;
}

void rewrites_the_heaps(bool non_uniform) {
    std::string why;
    const auto rewritten = sfr::bounded_descriptor_spirv(heaps(non_uniform).bytes(), &why);
    require(rewritten.has_value(), "the translator's heaps are rewritten: " + why);
    const Parsed out = parse(*rewritten);
    require(out.bound == 101, "one new id, the length constant (the module had the uint type)");
    // Capabilities: no descriptor indexing, dynamic indexing instead.
    std::vector<uint32_t> capabilities;
    for (const auto& in : out.of(17)) capabilities.push_back(in[1]);
    require(capabilities == std::vector<uint32_t>{Shader, SampledImageArrayDynamicIndexing, ImageQuery},
            "capabilities are Shader, SampledImageArrayDynamicIndexing and ImageQuery, in place");
    require(out.of(10).empty(), "the descriptor-indexing extension is gone");
    for (const auto& in : out.of(71)) require(in[2] != 5300, "no NonUniform decoration remains");
    // The four heaps are fixed arrays of 16; the storage buffer's array stays.
    const auto arrays = out.of(28);
    require(arrays.size() == 4, "four fixed arrays");
    const auto constants = out.of(43);
    require(constants.size() == 1 && constants[0][1] == 6 && constants[0][2] == 100 &&
            constants[0][3] == sfr::bounded_descriptor_count, "one length constant of 16, of the module's uint");
    std::map<uint32_t, uint32_t> elements;
    for (const auto& in : arrays) {
        require(in[3] == 100, "each array has the length constant");
        elements[in[1]] = in[2];
        require(out.position(100) < out.position(in[1]), "the length is declared before the array");
    }
    require(elements == std::map<uint32_t, uint32_t>{{20, 10}, {21, 11}, {22, 12}, {23, 13}},
            "the arrays keep their ids and element types");
    require(out.position(6) < out.position(100), "the uint type moved ahead of its constant");
    require(out.of(21).size() == 1, "the uint type is declared once");
    const auto runtime = out.of(29);
    require(runtime.size() == 1 && runtime[0][1] == 15, "the storage buffer's runtime array is untouched");
    // Bindings by image type; sets unchanged.
    require(out.decoration(40, 33) == 0 && out.decoration(41, 33) == 1 && out.decoration(42, 33) == 2,
            "2D, 2D array and cube heaps take bindings 0, 1 and 2");
    require(out.decoration(40, 34) == 0 && out.decoration(41, 34) == 0 && out.decoration(42, 34) == 0,
            "the texture heaps stay in set 0");
    require(out.decoration(43, 34) == 1 && out.decoration(43, 33) == 0, "the samplers stay set 1 binding 0");
    require(out.decoration(44, 34) == 2 && out.decoration(44, 33) == 0, "set 2 is untouched");
    // Rewriting again changes nothing but what it has already changed.
    const auto twice = sfr::bounded_descriptor_spirv(*rewritten, &why);
    require(twice.has_value() && *twice == *rewritten, "the rewrite is idempotent");
}

void adds_a_uint_type_when_missing() {
    Module m;
    m.op(17, {Shader});
    m.op(17, {RuntimeDescriptorArray});
    m.op(14, {0, 1});
    m.op(71, {43, 34, 1});
    m.op(71, {43, 33, 0});
    m.op(26, {13});
    m.op(29, {23, 13});
    m.op(32, {33, 0, 23});
    m.op(59, {33, 43, 0});
    std::string why;
    const auto rewritten = sfr::bounded_descriptor_spirv(m.bytes(), &why);
    require(rewritten.has_value(), "a sampler-only module is rewritten: " + why);
    const Parsed out = parse(*rewritten);
    require(out.bound == 102, "two new ids: the length and a uint type");
    const auto ints = out.of(21);
    require(ints.size() == 1 && ints[0][1] == 101 && ints[0][2] == 32 && ints[0][3] == 0, "a uint type is added");
    require(out.position(101) < out.position(100) && out.position(100) < out.position(23),
            "type, constant, array in order");
}

void leaves_a_module_without_heaps() {
    Module m;
    m.op(17, {Shader});
    m.op(17, {RuntimeDescriptorArray});
    m.op_string(10, {}, "SPV_EXT_descriptor_indexing");
    m.op(14, {0, 1});
    m.op(71, {44, 34, 2});
    m.op(71, {44, 33, 0});
    m.op(19, {2});
    std::string why;
    const auto rewritten = sfr::bounded_descriptor_spirv(m.bytes(), &why);
    require(rewritten.has_value(), "a module without heaps is accepted: " + why);
    const Parsed out = parse(*rewritten);
    require(out.bound == 100, "no new ids");
    require(out.of(17).size() == 1 && out.of(17)[0][1] == Shader, "only Shader remains");
    require(out.of(10).empty(), "no extension remains");
    require(out.of(28).empty() && out.of(43).empty(), "nothing is added");
}

void refuses_what_it_does_not_know() {
    std::string why;
    // A heap in another set.
    {
        Module m;
        m.op(17, {Shader});
        m.op(17, {RuntimeDescriptorArray});
        m.op(71, {40, 34, 2});
        m.op(71, {40, 33, 5});
        m.op(25, {10, 5, 1, 2, 0, 0, 1, 0});
        m.op(29, {20, 10});
        m.op(32, {30, 0, 20});
        m.op(59, {30, 40, 0});
        require(!sfr::bounded_descriptor_spirv(m.bytes(), &why), "a heap outside sets 0 and 1 is refused");
    }
    // A single texture in set 0, not a heap.
    {
        Module m;
        m.op(17, {Shader});
        m.op(71, {40, 34, 0});
        m.op(71, {40, 33, 0});
        m.op(25, {10, 5, 1, 2, 0, 0, 1, 0});
        m.op(32, {30, 0, 10});
        m.op(59, {30, 40, 0});
        require(!sfr::bounded_descriptor_spirv(m.bytes(), &why), "a lone texture in set 0 is refused");
    }
    // A 3D texture heap has no binding in the layout.
    {
        Module m;
        m.op(17, {Shader});
        m.op(17, {RuntimeDescriptorArray});
        m.op(71, {40, 34, 0});
        m.op(71, {40, 33, 0});
        m.op(25, {10, 5, 2, 2, 0, 0, 1, 0});
        m.op(29, {20, 10});
        m.op(32, {30, 0, 20});
        m.op(59, {30, 40, 0});
        require(!sfr::bounded_descriptor_spirv(m.bytes(), &why), "a 3D heap is refused");
    }
    // Non-uniform indexing needs a feature the fallback's drivers lack.
    {
        Module m;
        m.op(17, {Shader});
        m.op(17, {5307});  // SampledImageArrayNonUniformIndexing
        require(!sfr::bounded_descriptor_spirv(m.bytes(), &why), "non-uniform indexing is refused");
    }
    require(!sfr::bounded_descriptor_spirv(std::vector<uint8_t>(12, 0), &why), "a short buffer is refused");
    auto bad = heaps(false).bytes();
    bad[0] = 0;
    require(!sfr::bounded_descriptor_spirv(bad, &why), "a wrong magic is refused");
    auto truncated = heaps(false).bytes();
    truncated[truncated.size() - 2] = 3;  // OpFunctionEnd claims three words, at the end of the module
    require(!sfr::bounded_descriptor_spirv(truncated, &why), "a truncated module is refused");
}

// SFR_SHADER_PACK=<shaders.pack>: every SPIR-V shader of a real pack is
// rewritten, and none keeps a runtime array of images or samplers.
void rewrites_a_shader_pack() {
    const char* path = std::getenv("SFR_SHADER_PACK");
    if (!path || !*path) return;
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    require(data.size() >= 16, "the pack is readable");
    uint32_t count;
    std::memcpy(&count, data.data() + 12, 4);
    size_t at = 16, rewritten = 0, with_heaps = 0;
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
        const auto out = sfr::bounded_descriptor_spirv(
            std::span<const uint8_t>(data.data() + spirv_at, header[4]), &why);
        require(out.has_value(), "pack shader " + std::to_string(i) + " is rewritten: " + why);
        const Parsed parsed = parse(*out);
        for (const auto& in : parsed.of(17))
            require(in[1] != RuntimeDescriptorArray && in[1] != ShaderNonUniform, "no descriptor-indexing capability");
        require(parsed.of(10).empty(), "no extension");
        // SFR_BOUNDED_SPIRV_DUMP=<directory>: each module before and after,
        // for spirv-val.
        if (const char* dump = std::getenv("SFR_BOUNDED_SPIRV_DUMP"); dump && *dump) {
            const auto base = (std::filesystem::u8path(dump) / std::to_string(i)).string();
            std::ofstream(base + ".in.spv", std::ios::binary)
                .write(reinterpret_cast<const char*>(data.data() + spirv_at), header[4]);
            std::ofstream(base + ".out.spv", std::ios::binary)
                .write(reinterpret_cast<const char*>(out->data()), std::streamsize(out->size()));
        }
        uint32_t original_bound;
        std::memcpy(&original_bound, data.data() + spirv_at + 12, 4);
        if (parsed.bound > original_bound) ++with_heaps;
        ++rewritten;
    }
    std::cout << "pack shaders rewritten=" << rewritten << " with_heaps=" << with_heaps << '\n';
}
}  // namespace

int main() {
    try {
        rewrites_the_heaps(false);
        rewrites_the_heaps(true);
        adds_a_uint_type_when_missing();
        leaves_a_module_without_heaps();
        refuses_what_it_does_not_know();
        rewrites_a_shader_pack();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "spirv_bounded_descriptors tests passed\n";
    return 0;
}
