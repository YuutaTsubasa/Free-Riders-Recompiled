#pragma once

#include "spirv_bounded_descriptors.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace sfr {

// When each texture descriptor index last changed the view it holds, for the
// bounded layout's sets (native_renderer.cpp): a set made after that still
// holds what the index names now. Stamps rather than the views themselves,
// as a driver may give a new view the handle of one destroyed before it.
class BoundedTextureStamps {
public:
    explicit BoundedTextureStamps(size_t indices) : stamps_(indices, 0) {}
    // The view at this index changed.
    void touch(uint32_t index) {
        if (index < stamps_.size()) stamps_[index] = ++clock_;
    }
    // The time now: a set made now holds what every index names.
    [[nodiscard]] uint64_t now() const noexcept { return clock_; }
    // Whether none of these indices changed after the time made.
    template <class Indices>
    [[nodiscard]] bool unchanged_since(const Indices& indices, uint64_t made) const noexcept {
        for (const uint32_t index : indices)
            if (index < stamps_.size() && stamps_[index] > made) return false;
        return true;
    }
private:
    std::vector<uint64_t> stamps_;
    uint64_t clock_ = 0;
};

// One upload ring's bounded descriptor sets by what they hold, kept from one
// use of the ring to the next: the GPU has finished with a ring's sets when
// it comes round again, and a race frame binds the same few dozen pairs as
// the frame before. Writing them all again each frame (48 textures and 16
// samplers a set) was most of what the layout cost a weak driver.
//   texture sets: by the 2D texture index of each fetch constant (the 2D
//     array and cube elements always read as zero), and valid while those
//     indices keep their views;
//   sampler sets: by the sampler index of each fetch constant (a sampler
//     index never changes what it names).
// A set is never written again once made, as a draw recorded earlier in the
// ring's use may have bound it: a texture set whose indices changed is left
// in its pool (abandoned) and a new one made in its place. The ring starts
// over (its pools reset) when abandoned sets or the sets kept grow too many.
template <class Set>
class BoundedSetCache {
public:
    static constexpr uint32_t count = bounded_descriptor_count;
    using Key = std::array<uint32_t, count>;
    // Start over when this many sets were abandoned, or this many are kept.
    static constexpr size_t max_abandoned = 256, max_kept = 2048;

    struct Found {
        Set* set;     // where the set is (or goes, when make)
        bool make;    // no valid set: make one and store it there
    };
    // The texture set for these 2D indices, given when each changed.
    Found textures(const Key& key, const BoundedTextureStamps& stamps) {
        auto [entry, added] = textures_.try_emplace(key);
        if (!added && stamps.unchanged_since(key, entry->second.made)) return {&entry->second.set, false};
        if (!added) ++abandoned_;
        entry->second.made = stamps.now();
        return {&entry->second.set, true};
    }
    Found samplers(const Key& key) {
        auto [entry, added] = samplers_.try_emplace(key);
        return {&entry->second, added};
    }
    // Whether the ring should start over before its next use.
    [[nodiscard]] bool crowded() const noexcept {
        return abandoned_ >= max_abandoned || textures_.size() + samplers_.size() >= max_kept;
    }
    // The ring's pools were reset: none of the sets exist any more.
    void clear() {
        textures_.clear();
        samplers_.clear();
        abandoned_ = 0;
    }
    [[nodiscard]] size_t kept() const noexcept { return textures_.size() + samplers_.size(); }
    [[nodiscard]] size_t abandoned() const noexcept { return abandoned_; }

private:
    struct KeyHash {
        size_t operator()(const Key& key) const noexcept {
            uint64_t hash = 0xcbf29ce484222325ull;
            for (const uint32_t word : key) hash = (hash ^ word) * 0x100000001b3ull;
            return size_t(hash ^ (hash >> 29));
        }
    };
    struct TextureEntry {
        Set set{};
        uint64_t made = 0;
    };
    std::unordered_map<Key, TextureEntry, KeyHash> textures_;
    std::unordered_map<Key, Set, KeyHash> samplers_;
    size_t abandoned_ = 0;
};

}  // namespace sfr
