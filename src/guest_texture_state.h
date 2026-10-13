#pragma once
#include "texture_fetch.h"
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>

namespace sfr {
// Guest fetch-word snapshots, independent of the renderer's descriptor lifetime.
// Mutations and preparation use the same graphics ownership as native draws.
class GuestTextureState {
public:
    static constexpr uint32_t sampled_slots = 16;
    static constexpr uint32_t source_bytes = 384;
    static constexpr uint64_t dirty_mask = 0xFFFF0000ull;
    struct Slot {
        FetchWords words{};
        uint64_t version = 0;
    };

    void invalidate_all() noexcept { pending_ = dirty_mask; }
    void invalidate_slot(uint32_t index) {
        if (index >= 26) throw std::out_of_range("texture slot outside the original 26-slot domain");
        if (index < sampled_slots) pending_ |= slot_mask(index);
    }
    uint64_t pending_mask(uint64_t dirty) const noexcept {
        return pending_ | (dirty & dirty_mask);
    }

    // source begins at device+0x480. Only pending slots are read, including all
    // sixteen on first use. The caller clears consumed guest bits after success.
    uint32_t prepare(uint64_t dirty, std::span<const uint8_t, source_bytes> source) {
        const uint64_t refresh = pending_mask(dirty);
        if (!refresh) return 0;
        std::array<FetchWords, sampled_slots> next;
        uint64_t changed = 0;
        uint32_t bytes = 0;
        for (uint32_t index = 0; index < sampled_slots; ++index) {
            if (!(refresh & slot_mask(index))) continue;
            for (uint32_t word = 0; word < 6; ++word)
                next[index][word] = read_word(source, index * 6 + word);
            bytes += 24;
            if (!slots_[index].version || next[index] != slots_[index].words) {
                if (slots_[index].version == (std::numeric_limits<uint64_t>::max)())
                    throw std::overflow_error("texture fetch version exhausted");
                changed |= slot_mask(index);
            }
        }
        // Preflight every changed version before publishing any slot. Failure
        // leaves host invalidations and all previously published values intact.
        for (uint32_t index = 0; index < sampled_slots; ++index) {
            if (!(changed & slot_mask(index))) continue;
            slots_[index].words = next[index];
            ++slots_[index].version;
        }
        pending_ &= ~refresh;
        return bytes;
    }
    const Slot& slot(uint32_t index) const {
        if (index >= sampled_slots) throw std::out_of_range("texture slot is not sampled by the native draw");
        return slots_[index];
    }
    // Correctness mode only: find untracked writes, including clean slots.
    // Return a linear word index (0..95), or 96 when the snapshots agree.
    uint32_t first_mismatch(std::span<const uint8_t, source_bytes> source) const {
        for (uint32_t index = 0; index < sampled_slots; ++index)
            for (uint32_t word = 0; word < 6; ++word)
                if (slots_[index].words[word] != read_word(source, index * 6 + word))
                    return index * 6 + word;
        return sampled_slots * 6;
    }
private:
    static constexpr uint64_t slot_mask(uint32_t index) noexcept { return 0x80000000ull >> index; }
    static uint32_t read_word(std::span<const uint8_t, source_bytes> source, uint32_t word) noexcept {
        const uint8_t* bytes = source.data() + word * 4;
        return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
               (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
    }
    std::array<Slot, sampled_slots> slots_{};
    uint64_t pending_ = dirty_mask;
};
}
