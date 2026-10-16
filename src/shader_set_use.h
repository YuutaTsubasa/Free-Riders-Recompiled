#pragma once

#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <unordered_map>

namespace plume { struct RenderShader; }

namespace sfr {

// The descriptor sets (bit n: set n, for sets 0 to 7) a SPIR-V module
// decorates, which are the ones its code uses: the shader translator leaves
// unused resources out.
inline uint8_t spirv_descriptor_sets(std::span<const uint8_t> code) {
    const auto word = [&](size_t i) {
        uint32_t w;
        std::memcpy(&w, code.data() + i * 4, 4);
        return w;
    };
    const size_t words = code.size() / 4;
    uint8_t sets = 0;
    for (size_t i = 5; i < words;) {
        const uint32_t count = word(i) >> 16, opcode = word(i) & 0xffff;
        if (!count) break;
        constexpr uint32_t decorate = 71, descriptor_set = 34;
        if (opcode == decorate && count >= 4 && i + 3 < words && word(i + 2) == descriptor_set && word(i + 3) < 8)
            sets |= uint8_t(1u << word(i + 3));
        i += count;
    }
    return sets;
}

// Which sets each created shader uses, for MoltenVK (native_renderer.cpp).
class ShaderSetUse {
public:
    static void note(const plume::RenderShader* shader, std::span<const uint8_t> spirv) {
        std::lock_guard lock(mutex());
        map()[shader] = spirv_descriptor_sets(spirv);
    }
    // Sets a shader uses; all of them for one never noted.
    static uint8_t of(const plume::RenderShader* shader) {
        std::lock_guard lock(mutex());
        const auto found = map().find(shader);
        return found == map().end() ? uint8_t(0xff) : found->second;
    }
private:
    static std::mutex& mutex() { static std::mutex m; return m; }
    static std::unordered_map<const plume::RenderShader*, uint8_t>& map() {
        static std::unordered_map<const plume::RenderShader*, uint8_t> m;
        return m;
    }
};

}  // namespace sfr
