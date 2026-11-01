#include "spirv_dispatch_loop.h"
#include "spirv_interpreter.h"

#include <algorithm>
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
    std::vector<uint32_t> words{0x07230203u, 0x00010000u, 0, 200, 0};
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

std::vector<uint32_t> opcodes(const std::vector<uint8_t>& bytes) {
    std::vector<uint32_t> words(bytes.size() / 4), out;
    std::memcpy(words.data(), bytes.data(), bytes.size());
    for (size_t i = 5; i < words.size(); i += words[i] >> 16) {
        require((words[i] >> 16) != 0, "malformed output");
        out.push_back(words[i] & 0xffff);
    }
    return out;
}

size_t count(const std::vector<uint32_t>& ops, uint32_t opcode) {
    return size_t(std::count(ops.begin(), ops.end(), opcode));
}

// The shape XenosRecomp's shaders take after DXC: a loop whose header holds
// the registers and the pc as phis, a switch on the pc, cases that fall
// through or continue at another pc, and a break.
//   x, n: inputs; y: output. r is the register, i a counter.
//   case 0: r += 1; if (r > n) continue at 3 (forward); fall through
//   case 1: r = r * r; fall through
//   case 2: i += 1; if (i < n) continue at 1 (backward) else break
//   case 3: r = -r; break
//   y = r after the loop.
// computed_pc makes the continue from case 2 take the pc phi itself.
Module dispatch_shader(bool computed_pc = false) {
    Module m;
    m.op(17, {1});                    // Shader
    m.op(14, {0, 1});                 // Logical GLSL450
    m.op(15, {0, 30, 0x6e69616d, 0, 20, 22, 21});
    m.op(71, {20, 30, 0});            // Location 0
    m.op(71, {22, 30, 1});
    m.op(71, {21, 30, 0});
    m.op(19, {1});
    m.op(33, {2, 1});
    m.op(22, {3, 32});
    m.op(21, {4, 32, 0});
    m.op(20, {5});
    m.op(32, {6, 1, 3});              // Input float*
    m.op(32, {7, 3, 3});              // Output float*
    m.op(43, {4, 10, 0});
    m.op(43, {4, 11, 1});
    m.op(43, {4, 12, 2});
    m.op(43, {4, 13, 3});
    m.op(43, {3, 14, 0x3f800000});    // 1.0
    m.op(43, {3, 15, 0});             // 0.0
    m.op(59, {6, 20, 1});
    m.op(59, {6, 22, 1});
    m.op(59, {7, 21, 3});
    m.op(54, {1, 30, 0, 2});
    m.op(248, {40});
    m.op(61, {3, 41, 20});
    m.op(61, {3, 42, 22});
    m.op(249, {50});
    m.op(248, {50});                  // H
    m.op(245, {3, 51, 41, 40, 71, 60});
    m.op(245, {4, 52, 10, 40, 72, 60});
    m.op(245, {3, 53, 15, 40, 73, 60});
    m.op(246, {90, 60, 0});
    m.op(249, {55});
    m.op(248, {55});                  // D
    m.op(247, {80, 0});
    m.op(251, {52, 80, 0, 100, 1, 110, 2, 120, 3, 130});
    m.op(248, {100});                 // case 0
    m.op(129, {3, 101, 51, 14});
    m.op(186, {5, 102, 101, 42});
    m.op(247, {105, 0});
    m.op(250, {102, 104, 105});
    m.op(248, {104});
    m.op(249, {60});
    m.op(248, {105});
    m.op(249, {110});
    m.op(248, {110});                 // case 1
    m.op(245, {3, 111, 51, 55, 101, 105});
    m.op(133, {3, 112, 111, 111});
    m.op(249, {120});
    m.op(248, {120});                 // case 2
    m.op(245, {3, 121, 51, 55, 112, 110});
    m.op(129, {3, 122, 53, 14});
    m.op(184, {5, 123, 122, 42});
    m.op(247, {125, 0});
    m.op(250, {123, 124, 125});
    m.op(248, {124});
    m.op(249, {60});
    m.op(248, {125});
    m.op(249, {80});
    m.op(248, {130});                 // case 3
    m.op(127, {3, 131, 51});
    m.op(249, {80});
    m.op(248, {60});                  // C
    m.op(245, {3, 71, 101, 104, 121, 124});
    m.op(245, {4, 72, 13, 104, computed_pc ? 52u : 11u, 124});
    m.op(245, {3, 73, 53, 104, 122, 124});
    m.op(249, {50});
    m.op(248, {80});                  // S
    m.op(245, {3, 81, 15, 55, 121, 125, 131, 130});
    m.op(249, {90});
    m.op(248, {90});                  // M
    m.op(62, {21, 81});
    m.op(253, {});
    m.op(56, {});
    return m;
}

void same_results(const std::vector<uint8_t>& before, const std::vector<uint8_t>& after, const std::string& what,
                  size_t* compared = nullptr, uint32_t seeds = 12) {
    sfr::test::SpirvInterpreter a(before), b(after);
    for (uint32_t seed = 1; seed <= seeds; ++seed) {
        const auto x = a.run(seed), y = b.run(seed);
        require(x.has_value() == y.has_value(), what + ": one version did not finish, seed " + std::to_string(seed));
        if (!x) continue;
        require(*x == *y, what + ": the outputs differ for seed " + std::to_string(seed));
        if (compared) ++*compared;
    }
}

void takes_the_loop_out() {
    const auto before = dispatch_shader().bytes();
    std::string why;
    const auto after = sfr::without_dispatch_loop_spirv(before, &why);
    require(after.has_value(), "refused: " + why);
    const auto ops = opcodes(*after);
    require(count(ops, 245) == 0, "phis are left");
    // One loop is left, around cases 1 and 2 (the backward continue); each
    // case has a switch of its own.
    require(count(ops, 246) == 1, "not one loop around the backward continue");
    require(count(ops, 251) == 4, "not a guard for each case");
    size_t compared = 0;
    same_results(before, *after, "the hand-made shader", &compared);
    require(compared == 12, "the hand-made shader did not finish");
    if (const char* dump = std::getenv("SFR_DISPATCH_LOOP_DUMP"); dump && *dump) {
        std::filesystem::create_directories(std::filesystem::path(reinterpret_cast<const char8_t*>(dump)));
        std::ofstream(std::filesystem::path(reinterpret_cast<const char8_t*>(dump)) / "hand-before.spv", std::ios::binary)
            .write(reinterpret_cast<const char*>(before.data()), std::streamsize(before.size()));
        std::ofstream(std::filesystem::path(reinterpret_cast<const char8_t*>(dump)) / "hand-after.spv", std::ios::binary)
            .write(reinterpret_cast<const char*>(after->data()), std::streamsize(after->size()));
    }
}

void leaves_other_modules() {
    Module m;
    m.op(17, {1});
    m.op(14, {0, 1});
    const auto bytes = m.bytes();
    const auto result = sfr::without_dispatch_loop_spirv(bytes);
    require(result && *result == bytes, "a module without a dispatch loop changed");
}

void refuses_what_it_cannot_do() {
    std::string why;
    require(!sfr::without_dispatch_loop_spirv(dispatch_shader(true).bytes(), &why), "a continue to a computed pc was rewritten");
    require(why.find("computed") != std::string::npos, "the reason is not the computed pc: " + why);
    require(!sfr::without_dispatch_loop_spirv(std::vector<uint8_t>(7)), "a truncated module was rewritten");
}

// SFR_SHADER_PACK=<shaders.pack>: every shader of a real pack goes without
// its dispatch loop and gives the same outputs for the same inputs.
// SFR_DISPATCH_LOOP_DUMP=<directory> writes the rewritten modules there (for
// spirv-val).
void rewrites_a_shader_pack() {
    const char* path = std::getenv("SFR_SHADER_PACK");
    if (!path || !*path) return;
    const char* dump = std::getenv("SFR_DISPATCH_LOOP_DUMP");
    if (dump && *dump) std::filesystem::create_directories(std::filesystem::path(reinterpret_cast<const char8_t*>(dump)));
    std::ifstream file(std::filesystem::path(reinterpret_cast<const char8_t*>(path)), std::ios::binary);
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    require(data.size() >= 16, "the pack is readable");
    uint32_t shaders;
    std::memcpy(&shaders, data.data() + 12, 4);
    size_t at = 16, rewritten = 0, compared = 0, runs = 0;
    for (uint32_t i = 0; i < shaders; ++i) {
        uint32_t header[5];
        require(at + 20 <= data.size(), "the pack is whole");
        std::memcpy(header, data.data() + at, 20);
        at += 20;
        const size_t spirv_at = at + header[2] + header[3];
        at = spirv_at + header[4];
        require(at <= data.size(), "the pack is whole");
        if (!header[4]) continue;
        const std::vector<uint8_t> before(data.begin() + spirv_at, data.begin() + spirv_at + header[4]);
        std::string why;
        const auto after = sfr::without_dispatch_loop_spirv(before, &why);
        require(after.has_value(), "pack shader " + std::to_string(i) + " is rewritten: " + why);
        if (*after == before) continue;
        ++rewritten;
        const auto ops = opcodes(*after);
        require(count(ops, 245) == 0, "pack shader " + std::to_string(i) + " keeps phis");
        size_t finished = 0;
        same_results(before, *after, "pack shader " + std::to_string(i), &finished, 4);
        compared += finished != 0, runs += finished;
        if (dump && *dump) {
            std::ofstream out(std::filesystem::path(reinterpret_cast<const char8_t*>(dump)) / ("shader" + std::to_string(i) + ".spv"), std::ios::binary);
            out.write(reinterpret_cast<const char*>(after->data()), std::streamsize(after->size()));
        }
    }
    std::cout << "pack shaders without their dispatch loop=" << rewritten << " compared=" << compared
              << " runs=" << runs << '\n';
    require(compared == rewritten, "some rewritten pack shaders never finished a run");
}
}  // namespace

int main() {
    try {
        takes_the_loop_out();
        leaves_other_modules();
        refuses_what_it_cannot_do();
        rewrites_a_shader_pack();
    } catch (const std::exception& error) {
        std::cerr << "spirv dispatch loop test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "spirv dispatch loop tests passed\n";
    return 0;
}
