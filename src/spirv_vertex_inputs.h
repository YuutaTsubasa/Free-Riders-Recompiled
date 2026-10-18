#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace plume { struct RenderShader; }

namespace sfr {

// Every draw declares a vertex attribute for each of the twenty input
// locations a XenosRecomp vertex shader may read (native_vertex_layout.cpp),
// the ones its declaration lacks reading zeros. Vulkan guarantees only 16
// (maxVertexInputAttributes, and each location below it), which is what an
// old Mali reports: with twenty a Mali-G72's and a Mali-G76's GPU hung at the
// first draw (Issue #169). No shader reads more than fourteen, so on such a
// device each vertex shader takes its inputs at locations 0, 1, 2, ... in the
// order of their own locations, and its pipelines declare only those.
inline constexpr uint32_t vertex_input_locations = 20;

struct CompactedVertexInputs {
    std::vector<uint8_t> code;
    // The location each input had, by its new one; empty for a module that is
    // not a vertex shader (which is left as it is).
    std::vector<uint32_t> locations;
    bool vertex = false;
};

// The module with its vertex shader's input locations renumbered as above.
// Empty, with the reason in why, for a module this does not understand or one
// with more inputs than limit.
inline std::optional<CompactedVertexInputs> compact_vertex_inputs_spirv(std::span<const uint8_t> code,
                                                                        uint32_t limit, std::string* why = nullptr) {
    const auto refuse = [&](const std::string& reason) -> std::optional<CompactedVertexInputs> {
        if (why) *why = reason;
        return std::nullopt;
    };
    if (code.size() < 20 || code.size() % 4) return refuse("not a SPIR-V module");
    std::vector<uint32_t> words(code.size() / 4);
    std::memcpy(words.data(), code.data(), code.size());
    if (words[0] != 0x07230203u) return refuse("not a SPIR-V module (magic)");

    constexpr uint32_t op_entry_point = 15, op_type_int = 21, op_type_float = 22, op_type_vector = 23,
                       op_type_pointer = 32, op_variable = 59, op_decorate = 71, op_member_decorate = 72;
    constexpr uint32_t execution_vertex = 0, storage_input = 1;
    constexpr uint32_t decoration_builtin = 11, decoration_location = 30, decoration_component = 31;

    bool vertex = false, other_stage = false;
    std::unordered_set<uint32_t> scalar_or_vector;          // types one location holds
    std::unordered_map<uint32_t, uint32_t> input_pointers;  // Input pointer type -> pointee
    std::vector<std::pair<uint32_t, uint32_t>> inputs;      // (variable, pointer type)
    std::unordered_map<uint32_t, size_t> location_at;       // variable -> word of its Location
    std::unordered_set<uint32_t> builtins, with_component;
    for (size_t i = 5; i < words.size();) {
        const uint32_t count = words[i] >> 16, opcode = words[i] & 0xffff;
        if (!count || i + count > words.size()) return refuse("truncated instruction");
        const uint32_t* w = &words[i];
        switch (opcode) {
        case op_entry_point:
            if (count < 3) return refuse("malformed entry point");
            (w[1] == execution_vertex ? vertex : other_stage) = true;
            break;
        case op_type_int:
            if (count >= 3 && w[2] <= 32) scalar_or_vector.insert(w[1]);
            break;
        case op_type_float:
            if (count >= 3 && w[2] <= 32) scalar_or_vector.insert(w[1]);
            break;
        case op_type_vector:
            if (count >= 3 && scalar_or_vector.contains(w[2])) scalar_or_vector.insert(w[1]);
            break;
        case op_type_pointer:
            if (count >= 4 && w[2] == storage_input) input_pointers[w[1]] = w[3];
            break;
        case op_variable:
            if (count >= 4 && w[3] == storage_input) inputs.emplace_back(w[2], w[1]);
            break;
        case op_decorate:
            if (count >= 3 && w[2] == decoration_builtin) builtins.insert(w[1]);
            if (count >= 4 && w[2] == decoration_location) location_at[w[1]] = i + 3;
            if (count >= 3 && w[2] == decoration_component) with_component.insert(w[1]);
            break;
        case op_member_decorate:
            if (count >= 4 && w[3] == decoration_location) return refuse("a block decorated with locations");
            break;
        }
        i += count;
    }
    if (vertex && other_stage) return refuse("entry points of more than one stage");
    CompactedVertexInputs result;
    result.vertex = vertex;
    if (vertex) {
        std::vector<std::pair<uint32_t, size_t>> located;  // (location, word of its decoration)
        for (const auto& [variable, pointer] : inputs) {
            if (builtins.contains(variable)) continue;
            const auto at = location_at.find(variable);
            if (at == location_at.end()) return refuse("an input without a location");
            if (with_component.contains(variable)) return refuse("an input decorated with a component");
            const auto pointee = input_pointers.find(pointer);
            if (pointee == input_pointers.end() || !scalar_or_vector.contains(pointee->second))
                return refuse("an input of more than one location");
            located.emplace_back(words[at->second], at->second);
        }
        std::sort(located.begin(), located.end());
        for (size_t i = 1; i < located.size(); ++i)
            if (located[i].first == located[i - 1].first) return refuse("two inputs at one location");
        if (located.size() > limit)
            return refuse(std::to_string(located.size()) + " inputs, more than the " + std::to_string(limit) +
                          " vertex attributes the device has");
        for (size_t rank = 0; rank < located.size(); ++rank) {
            result.locations.push_back(located[rank].first);
            words[located[rank].second] = uint32_t(rank);
        }
    }
    result.code.resize(words.size() * 4);
    std::memcpy(result.code.data(), words.data(), result.code.size());
    return result;
}

// A pipeline's vertex elements for a vertex shader so compacted: for each of
// its inputs, the element at the location the input had, moved to the new
// one; the elements no input reads are left out.
template <class Element>
std::vector<Element> compact_vertex_elements(std::span<const Element> elements, std::span<const uint32_t> locations) {
    std::vector<Element> result;
    for (size_t rank = 0; rank < locations.size(); ++rank) {
        const auto found = std::find_if(elements.begin(), elements.end(),
                                        [&](const Element& element) { return element.location == locations[rank]; });
        if (found == elements.end()) continue;
        result.push_back(*found);
        result.back().location = uint32_t(rank);
    }
    return result;
}

// The original input locations of each vertex shader made from a compacted
// module (by its shader object), for the pipelines made with it.
class VertexInputUse {
public:
    static void note(const plume::RenderShader* shader, std::vector<uint32_t> locations) {
        std::lock_guard lock(mutex());
        map()[shader] = std::move(locations);
    }
    // Empty for a shader never noted: its pipelines take every element.
    static std::optional<std::vector<uint32_t>> of(const plume::RenderShader* shader) {
        std::lock_guard lock(mutex());
        const auto found = map().find(shader);
        if (found == map().end()) return std::nullopt;
        return found->second;
    }
private:
    static std::mutex& mutex() { static std::mutex m; return m; }
    static std::unordered_map<const plume::RenderShader*, std::vector<uint32_t>>& map() {
        static std::unordered_map<const plume::RenderShader*, std::vector<uint32_t>> m;
        return m;
    }
};

}  // namespace sfr
