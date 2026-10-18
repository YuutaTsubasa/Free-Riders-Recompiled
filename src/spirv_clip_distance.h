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

// The same vertex shader without its clip distance. XenosRecomp gives every
// vertex shader an SV_ClipDistance output (the guest's user clip plane),
// which the SPIR-V writes through an access chain into gl_ClipDistance. A
// device without shaderClipDistance may not take that capability, and Mali
// drivers (Issues #166, #169: Mali-G715 and G720) crash compiling it. The
// output becomes a private variable the shader still writes but nothing
// reads: the capability and the BuiltIn decoration go (and, before SPIR-V
// 1.4, the entry point's interface entry), and the pointer types that named the Output storage
// class get Private twins. Empty, with the reason in why, for a module this
// does not understand; a module without a clip distance comes back as it is.
inline std::optional<std::vector<uint8_t>> without_clip_distance_spirv(std::span<const uint8_t> code,
                                                                       std::string* why = nullptr) {
    const auto refuse = [&](const std::string& reason) -> std::optional<std::vector<uint8_t>> {
        if (why) *why = reason;
        return std::nullopt;
    };
    if (code.size() < 20 || code.size() % 4) return refuse("not a SPIR-V module");
    std::vector<uint32_t> words(code.size() / 4);
    std::memcpy(words.data(), code.data(), code.size());
    if (words[0] != 0x07230203u) return refuse("not a SPIR-V module (magic)");

    constexpr uint32_t op_entry_point = 15, op_capability = 17, op_type_pointer = 32, op_variable = 59,
                       op_load = 61, op_access_chain = 65, op_in_bounds_access_chain = 66, op_decorate = 71,
                       op_member_decorate = 72;
    constexpr uint32_t decoration_builtin = 11, builtin_clip_distance = 3, capability_clip_distance = 32;
    constexpr uint32_t storage_output = 3, storage_private = 6;

    struct Instruction { size_t at; uint32_t count, opcode; };
    std::vector<Instruction> instructions;
    std::unordered_set<uint32_t> clip;                      // variables decorated ClipDistance
    std::unordered_map<uint32_t, uint32_t> output_pointers; // Output pointer type -> pointee
    for (size_t i = 5; i < words.size();) {
        const uint32_t count = words[i] >> 16, opcode = words[i] & 0xffff;
        if (!count || i + count > words.size()) return refuse("truncated instruction");
        const uint32_t* w = &words[i];
        if (opcode == op_decorate && count >= 4 && w[2] == decoration_builtin && w[3] == builtin_clip_distance)
            clip.insert(w[1]);
        if (opcode == op_member_decorate && count >= 5 && w[3] == decoration_builtin && w[4] == builtin_clip_distance)
            return refuse("clip distance in a block");
        if (opcode == op_type_pointer && count >= 4 && w[2] == storage_output) output_pointers[w[1]] = w[3];
        instructions.push_back({i, count, opcode});
        i += count;
    }
    if (clip.empty()) return std::vector<uint8_t>(code.begin(), code.end());

    // The pointer types the clip distance variables and the access chains
    // into them use, each to get a Private twin.
    std::unordered_map<uint32_t, uint32_t> twin;  // Output pointer type -> Private one
    uint32_t bound = words[3];
    const auto private_pointer = [&](uint32_t type) -> std::optional<uint32_t> {
        if (!output_pointers.contains(type)) return std::nullopt;
        auto [it, added] = twin.try_emplace(type, 0);
        if (added) it->second = bound++;
        return it->second;
    };
    std::unordered_set<uint32_t> chains;
    for (const auto& in : instructions) {
        const uint32_t* w = &words[in.at];
        if (in.opcode == op_variable && clip.contains(w[2])) {
            if (in.count != 4 || w[3] != storage_output) return refuse("clip distance that is not a plain output");
            if (!private_pointer(w[1])) return refuse("clip distance of an unknown pointer type");
        }
        if ((in.opcode == op_access_chain || in.opcode == op_in_bounds_access_chain) && in.count >= 4 &&
            clip.contains(w[3])) {
            if (!private_pointer(w[1])) return refuse("access chain of an unknown pointer type");
            chains.insert(w[2]);
        }
        if (in.opcode == op_load && in.count >= 4 && clip.contains(w[3])) return refuse("clip distance is read");
    }

    std::vector<uint32_t> out(words.begin(), words.begin() + 5);
    out.reserve(words.size() + 8);
    for (const auto& in : instructions) {
        const uint32_t* w = &words[in.at];
        if (in.opcode == op_capability && w[1] == capability_clip_distance) continue;
        if (in.opcode == op_decorate && clip.contains(w[1])) continue;
        // From SPIR-V 1.4 the interface lists every global the entry point
        // uses, private ones too; before it, only inputs and outputs.
        if (in.opcode == op_entry_point && words[1] < 0x00010400u) {
            // Execution model, entry point, its name (a nul-terminated string
            // padded to words), then the interface.
            size_t name = 3;
            while (name < in.count && std::memchr(&w[name], 0, 4) == nullptr) ++name;
            const size_t first = name + 1;
            if (first > in.count) return refuse("malformed entry point");
            std::vector<uint32_t> kept(w, w + first);
            for (size_t k = first; k < in.count; ++k)
                if (!clip.contains(w[k])) kept.push_back(w[k]);
            kept[0] = (uint32_t(kept.size()) << 16) | op_entry_point;
            out.insert(out.end(), kept.begin(), kept.end());
            continue;
        }
        if (in.opcode == op_type_pointer && twin.contains(w[1])) {
            out.insert(out.end(), w, w + in.count);
            out.insert(out.end(), {(4u << 16) | op_type_pointer, twin.at(w[1]), storage_private, w[3]});
            continue;
        }
        const size_t start = out.size();
        out.insert(out.end(), w, w + in.count);
        if (in.opcode == op_variable && clip.contains(w[2])) {
            out[start + 1] = twin.at(w[1]);
            out[start + 3] = storage_private;
        }
        if ((in.opcode == op_access_chain || in.opcode == op_in_bounds_access_chain) && chains.contains(w[2]))
            out[start + 1] = twin.at(w[1]);
    }
    out[3] = bound;
    std::vector<uint8_t> bytes(out.size() * 4);
    std::memcpy(bytes.data(), out.data(), bytes.size());
    return bytes;
}

}  // namespace sfr
