#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sfr {

// The renderer's layout for a Vulkan driver without descriptor indexing
// (Issue #134, native_renderer.cpp): instead of one partly filled array of
// 4096 textures and one of 256 samplers, each draw binds a set of its own
// that holds just the textures and samplers its sixteen fetch constants
// name, at their fetch-constant numbers:
//   set 0, binding 0: 16 2D textures
//   set 0, binding 1: 16 2D array textures
//   set 0, binding 2: 16 cube textures
//   set 1, binding 0: 16 samplers
// The translator's three texture heaps alias set 0 binding 0 as runtime
// arrays (vulkan_shader_source.cpp); each becomes a fixed array of its own
// binding, as a set without partially bound descriptors must hold a texture
// of the shader's own type in every element.
inline constexpr uint32_t bounded_descriptor_count = 16;

// The elements of each texture binding. The full layout above has
// bounded_descriptor_count of each. A driver allowing fewer textures a stage
// (a Mali-G72's 16) gets the compact one: fewer 2D textures, which the
// renderer fills with each draw's distinct textures after a zero-reading
// first element, and one 2D array and one cube texture, which read as zero
// anyway (only 2D textures exist, native_renderer.cpp).
struct BoundedLengths {
    uint32_t texture_2d = bounded_descriptor_count;
    uint32_t texture_2d_array = bounded_descriptor_count;
    uint32_t texture_cube = bounded_descriptor_count;
    [[nodiscard]] bool compact() const noexcept { return texture_2d_array < bounded_descriptor_count; }
    [[nodiscard]] uint32_t textures() const noexcept { return texture_2d + texture_2d_array + texture_cube; }
    [[nodiscard]] uint32_t of(uint32_t binding) const noexcept {
        return binding == 0 ? texture_2d : binding == 1 ? texture_2d_array : texture_cube;
    }
};

// The layout for this many textures a stage: the full one if it fits, else
// the compact one (empty if not even that fits; it needs eight 2D textures).
inline std::optional<BoundedLengths> bounded_lengths(uint32_t textures_per_stage) {
    if (textures_per_stage >= 3 * bounded_descriptor_count) return BoundedLengths{};
    if (textures_per_stage < 8 + 2) return std::nullopt;
    return BoundedLengths{(std::min)(textures_per_stage - 2, bounded_descriptor_count), 1, 1};
}

// The same module for that layout: the runtime arrays of images and samplers
// in sets 0 and 1 become fixed arrays (of bounded_descriptor_count samplers,
// and of textures as many as lengths gives their binding), the texture
// heaps move to their bindings by image type, and the descriptor-indexing
// capabilities, extension and NonUniform decorations are dropped (a fixed
// array indexed by a dynamically uniform value needs only the Vulkan 1.0
// shaderSampledImageArrayDynamicIndexing feature). The renderer then puts
// fetch-constant numbers, not heap indices, in the shared constants the
// shader indexes with. Empty, with the reason in why, for a module this does
// not understand; anything else is copied as it is.
inline std::optional<std::vector<uint8_t>> bounded_descriptor_spirv(std::span<const uint8_t> code,
                                                                    std::string* why = nullptr,
                                                                    const BoundedLengths& lengths = {}) {
    const auto refuse = [&](const std::string& reason) -> std::optional<std::vector<uint8_t>> {
        if (why) *why = reason;
        return std::nullopt;
    };
    if (code.size() < 20 || code.size() % 4) return refuse("not a SPIR-V module");
    std::vector<uint32_t> words(code.size() / 4);
    std::memcpy(words.data(), code.data(), code.size());
    if (words[0] != 0x07230203u) return refuse("not a SPIR-V module (magic)");

    constexpr uint32_t op_extension = 10, op_capability = 17, op_type_int = 21, op_type_image = 25,
                       op_type_sampler = 26, op_type_sampled_image = 27, op_type_array = 28,
                       op_type_runtime_array = 29, op_type_pointer = 32, op_constant = 43, op_variable = 59,
                       op_decorate = 71;
    constexpr uint32_t decoration_binding = 33, decoration_descriptor_set = 34, decoration_non_uniform = 5300;
    constexpr uint32_t capability_sampled_image_array_dynamic_indexing = 29, capability_shader_non_uniform = 5301,
                       capability_runtime_descriptor_array = 5302;
    constexpr uint32_t storage_uniform_constant = 0, dim_2d = 1, dim_cube = 3;

    struct Instruction { size_t at; uint32_t count, opcode; };
    std::vector<Instruction> instructions;
    struct Image { uint32_t dim, arrayed; };
    std::unordered_map<uint32_t, Image> images;
    std::unordered_set<uint32_t> samplers;
    std::unordered_map<uint32_t, uint32_t> runtime_arrays;  // id -> element type
    std::unordered_map<uint32_t, uint32_t> fixed_arrays;    // id -> element type
    std::unordered_map<uint32_t, uint32_t> pointers;        // UniformConstant pointer id -> pointee
    std::unordered_map<uint32_t, uint32_t> sets, binding_at;  // variable -> set, -> word of its Binding
    std::vector<std::pair<uint32_t, uint32_t>> variables;   // (pointer type, id) in UniformConstant
    size_t uint_at = 0;  // instruction index + 1 of OpTypeInt 32 0
    uint32_t uint_type = 0;
    bool has_dynamic_indexing = false;
    for (size_t i = 5; i < words.size();) {
        const uint32_t count = words[i] >> 16, opcode = words[i] & 0xffff;
        if (!count || i + count > words.size()) return refuse("truncated instruction");
        const uint32_t* w = &words[i];
        switch (opcode) {
        case op_capability:
            if (count < 2) return refuse("malformed capability");
            // The other descriptor-indexing capabilities need features the
            // fallback is for drivers without.
            if (w[1] >= 5303 && w[1] <= 5312)
                return refuse("capability " + std::to_string(w[1]) + " needs descriptor indexing");
            if (w[1] == capability_sampled_image_array_dynamic_indexing) has_dynamic_indexing = true;
            break;
        case op_type_int:
            if (count >= 4 && w[2] == 32 && w[3] == 0 && !uint_type) {
                uint_type = w[1];
                uint_at = instructions.size() + 1;
            }
            break;
        case op_type_image:
            if (count < 9) return refuse("malformed image type");
            images[w[1]] = {w[3], w[5]};
            break;
        case op_type_sampler:
            samplers.insert(w[1]);
            break;
        case op_type_sampled_image:
            break;
        case op_type_array:
            if (count < 4) return refuse("malformed array");
            fixed_arrays[w[1]] = w[2];
            break;
        case op_type_runtime_array:
            if (count < 3) return refuse("malformed runtime array");
            runtime_arrays[w[1]] = w[2];
            break;
        case op_type_pointer:
            if (count >= 4 && w[2] == storage_uniform_constant) pointers[w[1]] = w[3];
            break;
        case op_variable:
            if (count >= 4 && w[3] == storage_uniform_constant) variables.emplace_back(w[1], w[2]);
            break;
        case op_decorate:
            if (count >= 4 && w[2] == decoration_descriptor_set) sets[w[1]] = w[3];
            if (count >= 4 && w[2] == decoration_binding) binding_at[w[1]] = uint32_t(i + 3);
            break;
        }
        instructions.push_back({i, count, opcode});
        i += count;
    }

    // Runtime arrays of images or samplers become fixed arrays; any other
    // runtime array (a storage buffer's last member) is left as it is.
    std::unordered_set<uint32_t> bounded;
    for (const auto& [id, element_type] : runtime_arrays) {
        const uint32_t element = element_type;  // a lambda below captures it (not a structured binding)
        if (images.contains(element) || samplers.contains(element)) bounded.insert(id);
        // A combined image sampler array would need a binding type the
        // fallback layout does not have.
        else if (const auto found = std::find_if(instructions.begin(), instructions.end(), [&](const Instruction& in) {
                     return in.opcode == op_type_sampled_image && words[in.at + 1] == element;
                 }); found != instructions.end())
            return refuse("runtime array of combined image samplers");
    }
    // Every resource of sets 0 and 1 must be one of the heaps, which move to
    // their bindings by type.
    std::vector<std::pair<uint32_t, uint32_t>> rebinding;  // word, binding
    for (const auto& [pointer, id] : variables) {
        const auto set = sets.find(id);
        const auto pointee = pointers.find(pointer);
        const bool heap = pointee != pointers.end() && bounded.contains(pointee->second);
        if (set == sets.end() || set->second > 1) {
            if (heap) return refuse("descriptor heap outside sets 0 and 1");
            continue;
        }
        if (!heap) {
            // Already a fixed array (a module rewritten before): left as it is.
            const auto fixed = pointee == pointers.end() ? fixed_arrays.end() : fixed_arrays.find(pointee->second);
            if (fixed != fixed_arrays.end() && (images.contains(fixed->second) || samplers.contains(fixed->second)))
                continue;
            return refuse("set " + std::to_string(set->second) + " holds a resource that is not a descriptor heap");
        }
        const uint32_t element = runtime_arrays.at(pointee->second);
        const auto binding = binding_at.find(id);
        if (binding == binding_at.end()) return refuse("descriptor heap without a binding");
        uint32_t target;
        if (samplers.contains(element)) {
            if (set->second != 1) return refuse("sampler heap outside set 1");
            target = 0;
        } else {
            if (set->second != 0) return refuse("texture heap outside set 0");
            const Image image = images.at(element);
            if (image.dim == dim_2d && !image.arrayed) target = 0;
            else if (image.dim == dim_2d && image.arrayed) target = 1;
            else if (image.dim == dim_cube && !image.arrayed) target = 2;
            else return refuse("texture heap of image dimension " + std::to_string(image.dim));
        }
        rebinding.emplace_back(binding->second, target);
    }
    for (const auto& [word, binding] : rebinding) words[word] = binding;

    // Each fixed array's length by its element type, and a constant for each
    // distinct length.
    const auto length_of = [&](uint32_t element) {
        const auto image = images.find(element);
        if (image == images.end()) return bounded_descriptor_count;
        if (image->second.dim == dim_2d) return image->second.arrayed ? lengths.texture_2d_array : lengths.texture_2d;
        return image->second.dim == dim_cube ? lengths.texture_cube : bounded_descriptor_count;
    };
    uint32_t bound = words[3];
    std::vector<std::pair<uint32_t, uint32_t>> length_constants;  // length, constant id
    for (const auto& in : instructions)
        if (in.opcode == op_type_runtime_array && bounded.contains(words[in.at + 1])) {
            const uint32_t length = length_of(words[in.at + 2]);
            if (std::none_of(length_constants.begin(), length_constants.end(),
                             [&](const auto& constant) { return constant.first == length; }))
                length_constants.emplace_back(length, bound++);
        }
    const bool new_uint = !bounded.empty() && !uint_type;
    if (new_uint) uint_type = bound++;
    std::vector<uint32_t> out(words.begin(), words.begin() + 5);
    out.reserve(words.size() + 16);
    bool capability_added = has_dynamic_indexing || bounded.empty(), length_emitted = false, uint_emitted = false;
    for (size_t index = 0; index < instructions.size(); ++index) {
        const auto& in = instructions[index];
        const uint32_t* w = &words[in.at];
        if (in.opcode == op_capability &&
            (w[1] == capability_runtime_descriptor_array || w[1] == capability_shader_non_uniform)) {
            if (!capability_added) {
                out.insert(out.end(), {(2u << 16) | op_capability, capability_sampled_image_array_dynamic_indexing});
                capability_added = true;
            }
            continue;
        }
        if (in.opcode == op_extension) {
            const char* name = reinterpret_cast<const char*>(w + 1);
            if (std::string(name, strnlen(name, size_t(in.count - 1) * 4)) == "SPV_EXT_descriptor_indexing") continue;
        }
        if (in.opcode == op_decorate && w[2] == decoration_non_uniform) continue;
        if (in.opcode == op_type_int && uint_emitted && index + 1 == uint_at) continue;  // hoisted
        if (in.opcode == op_type_runtime_array && bounded.contains(w[1])) {
            // The array's length is a constant of a 32-bit unsigned type
            // declared before it: the module's own, moved up if it came later
            // (it refers to nothing), or a new one.
            if (!uint_emitted && (new_uint || uint_at > index + 1)) {
                out.insert(out.end(), {(4u << 16) | op_type_int, uint_type, 32u, 0u});
            }
            uint_emitted = true;
            if (!length_emitted) {
                for (const auto& [length, id] : length_constants)
                    out.insert(out.end(), {(4u << 16) | op_constant, uint_type, id, length});
                length_emitted = true;
            }
            const uint32_t length = length_of(w[2]);
            const auto constant = std::find_if(length_constants.begin(), length_constants.end(),
                                               [&](const auto& c) { return c.first == length; });
            out.insert(out.end(), {(4u << 16) | op_type_array, w[1], w[2], constant->second});
            continue;
        }
        if (in.opcode == op_type_int && index + 1 == uint_at) uint_emitted = true;
        out.insert(out.end(), w, w + in.count);
    }
    if (!capability_added) return refuse("module declares no capabilities");
    out[3] = bound;
    std::vector<uint8_t> bytes(out.size() * 4);
    std::memcpy(bytes.data(), out.data(), bytes.size());
    return bytes;
}

}  // namespace sfr
