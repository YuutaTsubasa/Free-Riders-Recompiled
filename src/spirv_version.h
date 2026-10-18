#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sfr {

// The same shader as SPIR-V 1.3, the newest a Vulkan 1.1 device takes. DXC
// writes the game's shaders as SPIR-V 1.5 (Vulkan 1.2), but they use nothing
// newer than 1.3 except for the entry point's interface: from 1.4 it lists
// every global the entry point uses, and before it only the inputs and
// outputs, so the rest leave it. Empty, with the reason in why, for a module
// that uses what SPIR-V 1.3 does not have (as far as this checks: the 1.4
// and 1.5 instructions, a select of composites, and the 1.5 capabilities); a
// module at 1.3 or older comes back as it is.
inline std::optional<std::vector<uint8_t>> spirv_1_3(std::span<const uint8_t> code, std::string* why = nullptr) {
    const auto refuse = [&](const std::string& reason) -> std::optional<std::vector<uint8_t>> {
        if (why) *why = reason;
        return std::nullopt;
    };
    if (code.size() < 20 || code.size() % 4) return refuse("not a SPIR-V module");
    std::vector<uint32_t> words(code.size() / 4);
    std::memcpy(words.data(), code.data(), code.size());
    if (words[0] != 0x07230203u) return refuse("not a SPIR-V module (magic)");
    constexpr uint32_t version_1_3 = 0x00010300u;
    if (words[1] <= version_1_3) return std::vector<uint8_t>(code.begin(), code.end());

    constexpr uint32_t op_memory_model = 14, op_entry_point = 15, op_capability = 17, op_type_bool = 20,
                       op_type_int = 21, op_type_float = 22, op_type_vector = 23, op_variable = 59,
                       op_select = 169, op_copy_logical = 400, op_ptr_equal = 401, op_ptr_not_equal = 402,
                       op_ptr_diff = 403, op_decorate_string = 5632, op_member_decorate_string = 5633;
    constexpr uint32_t storage_input = 1, storage_output = 3;
    constexpr uint32_t memory_model_vulkan = 3;
    constexpr uint32_t capability_vulkan_memory_model = 5345, capability_physical_storage_buffer = 5347;

    struct Instruction { size_t at; uint32_t count, opcode; };
    std::vector<Instruction> instructions;
    std::unordered_map<uint32_t, uint32_t> storage;  // variable -> storage class
    std::unordered_set<uint32_t> selectable;          // scalar and vector types
    for (size_t i = 5; i < words.size();) {
        const uint32_t count = words[i] >> 16, opcode = words[i] & 0xffff;
        if (!count || i + count > words.size()) return refuse("truncated instruction");
        const uint32_t* w = &words[i];
        switch (opcode) {
        case op_capability:
            if (count >= 2 && (w[1] == capability_vulkan_memory_model || w[1] == capability_physical_storage_buffer))
                return refuse("capability " + std::to_string(w[1]) + " is SPIR-V 1.5");
            break;
        case op_memory_model:
            if (count >= 3 && w[2] == memory_model_vulkan) return refuse("the Vulkan memory model is SPIR-V 1.5");
            break;
        case op_type_bool: case op_type_int: case op_type_float: case op_type_vector:
            if (count >= 2) selectable.insert(w[1]);
            break;
        case op_variable:
            if (count >= 4) storage[w[2]] = w[3];
            break;
        case op_select:
            if (count >= 2 && !selectable.contains(w[1])) return refuse("a select of composites is SPIR-V 1.4");
            break;
        case op_copy_logical: case op_ptr_equal: case op_ptr_not_equal: case op_ptr_diff:
        case op_decorate_string: case op_member_decorate_string:
            return refuse("instruction " + std::to_string(opcode) + " is SPIR-V 1.4");
        }
        instructions.push_back({i, count, opcode});
        i += count;
    }

    std::vector<uint32_t> out(words.begin(), words.begin() + 5);
    out[1] = version_1_3;
    out.reserve(words.size());
    for (const auto& in : instructions) {
        const uint32_t* w = &words[in.at];
        if (in.opcode != op_entry_point) {
            out.insert(out.end(), w, w + in.count);
            continue;
        }
        // Execution model, entry point, its name (a nul-terminated string
        // padded to words), then the interface.
        size_t name = 3;
        while (name < in.count && std::memchr(&w[name], 0, 4) == nullptr) ++name;
        const size_t first = name + 1;
        if (first > in.count) return refuse("malformed entry point");
        std::vector<uint32_t> kept(w, w + first);
        for (size_t k = first; k < in.count; ++k) {
            const auto variable = storage.find(w[k]);
            if (variable == storage.end()) return refuse("an interface entry that is not a variable");
            if (variable->second == storage_input || variable->second == storage_output) kept.push_back(w[k]);
        }
        kept[0] = (uint32_t(kept.size()) << 16) | op_entry_point;
        out.insert(out.end(), kept.begin(), kept.end());
    }
    std::vector<uint8_t> bytes(out.size() * 4);
    std::memcpy(bytes.data(), out.data(), bytes.size());
    return bytes;
}

}  // namespace sfr
