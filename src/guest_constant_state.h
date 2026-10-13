#pragma once
#include "word_swap.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>

namespace sfr {
// One device stage's version and optional CPU shadow. Updates are serialized
// with native draws by the graphics lock. GPU uploads consume their source
// synchronously; the render thread must never retain mutable source storage.
class GuestConstantState {
public:
    static constexpr uint32_t words = 1024;
    using Values = std::array<uint32_t, words>;
    // The original D3D device's 64-bit mask describes 64-byte groups,
    // most significant bit first (82508890). Inline game setters use this
    // same mask, bypassing Set*ShaderConstantF entirely.
    void invalidate_mask(uint64_t mask) {
        if (!mask) return;
        mark_pending();
        groups_ |= mask;
    }
    void invalidate(uint32_t first = 0, uint32_t count = words) {
        if (first > words || count > words - first)
            throw std::out_of_range("shader constant update exceeds stage");
        if (!count) return;
        mark_pending();
        first_ = (std::min)(first_, first);
        end_ = (std::max)(end_, first + count);
    }
    // Read the union of dirty group runs and the explicit word interval. Gaps
    // between mask bits remain untouched; overlap/adjacency converts only once.
    // Return CPU conversion bytes, independently of the immutable GPU upload.
    uint32_t prepare(std::span<const uint8_t, words * 4> source) {
        if (!shadow_pending()) return 0;
        uint32_t bytes = 0, run_first = words, run_end = 0;
        const auto flush = [&] {
            if (run_first >= run_end) return;
            const uint32_t size = (run_end - run_first) * 4;
            swap_words_into({reinterpret_cast<uint8_t*>(values_.data() + run_first), size},
                            source.subspan(run_first * 4, size));
            bytes += size;
        };
        const auto append = [&](uint32_t first, uint32_t end) {
            if (run_first < run_end && first <= run_end) {
                run_end = (std::max)(run_end, end);
            } else {
                flush();
                run_first = first;
                run_end = end;
            }
        };
        bool explicit_pending = first_ < end_;
        uint64_t groups = groups_;
        while (groups) {
            const uint32_t first_group = std::countl_zero(groups);
            const uint32_t end_group = first_group + std::countl_one(groups << first_group);
            const uint32_t first = first_group * 16;
            if (explicit_pending && first_ <= first) {
                append(first_, end_);
                explicit_pending = false;
            }
            append(first, end_group * 16);
            groups = end_group == 64 ? 0 : groups & (~uint64_t{0} >> end_group);
        }
        if (explicit_pending) append(first_, end_);
        flush();
        groups_ = 0;
        first_ = words;
        end_ = 0;
        publish_version();
        return bytes;
    }
    // Direct-upload mode can consume update metadata without reading source or
    // materializing values(). Keep shadow dirtiness for a later sparse prepare,
    // especially the independent verifier. Its full comparison must never use
    // a shadow populated by the direct full-stage upload it is checking.
    uint64_t publish_version() noexcept {
        version_pending_ = false;
        return version_;
    }
    // Correctness mode only: detect writers missing from the hook contract.
    // Do not use content comparison to choose reuse in performance mode.
    uint32_t first_mismatch(std::span<const uint8_t, words * 4> source) const {
        for (uint32_t i = 0; i < words; ++i) {
            uint32_t value;
            std::memcpy(&value, source.data() + i * 4, 4);
            if (values_[i] != __builtin_bswap32(value)) return i;
        }
        return words;
    }
    // Only prepare() materializes this shadow; publish_version() does not.
    const Values& values() const noexcept { return values_; }
    uint64_t version() const noexcept { return version_; }
private:
    bool shadow_pending() const noexcept { return groups_ || first_ < end_; }
    void mark_pending() {
        if (version_pending_) return;
        if (version_ == (std::numeric_limits<uint64_t>::max)())
            throw std::overflow_error("shader constant version exhausted");
        ++version_;
        version_pending_ = true;
    }
    Values values_{};
    uint64_t version_ = 1;
    uint64_t groups_ = ~uint64_t{0};
    uint32_t first_ = words, end_ = 0;
    bool version_pending_ = true;
};

// The bool and integer setters share one device dirty bit. Refresh both
// groups together before consuming it, including shader-embedded updates.
// Access is serialized with draws in the same way as GuestConstantState.
class GuestShortConstantState {
public:
    static constexpr uint32_t source_bytes = 96;
    static constexpr uint64_t dirty_mask = 1ull << 56;
    using LoopValues = std::array<int32_t, 64>;
    void invalidate() {
        if (version_ == (std::numeric_limits<uint64_t>::max)())
            throw std::overflow_error("short shader constant version exhausted");
        ++version_;
        dirty_ = true;
    }
    void invalidate_mask(uint64_t mask) {
        if (mask & dirty_mask) invalidate();
    }
    // source begins at device +10112. The original VS loop setter uses stwu,
    // so i0 is at +10144 and i15 at +10204. Only two bool words and
    // those sixteen loop words are read; PS loops start beyond this span.
    uint32_t prepare(std::span<const uint8_t, source_bytes> source) {
        if (!dirty_) return 0;
        for (uint32_t i = 0; i < words_.size(); ++i) words_[i] = read_word(source, source_offset(i));
        booleans_ = (words_[0] & 0xffffu) | ((words_[1] & 0xffffu) << 16);
        for (uint32_t i = 0; i < 16; ++i)
            for (uint32_t lane = 0; lane < 4; ++lane) {
                const uint32_t byte = (words_[i + 2] >> (lane * 8)) & 0xffu;
                loops_[i * 4 + lane] = int32_t(byte) - (byte & 0x80u ? 256 : 0);
            }
        dirty_ = false;
        return uint32_t(words_.size() * 4);
    }
    // Correctness mode checks every read word, not just the bool bits used
    // by shaders. Return a byte offset relative to device +10112.
    uint32_t first_mismatch(std::span<const uint8_t, source_bytes> source) const {
        for (uint32_t i = 0; i < words_.size(); ++i)
            if (words_[i] != read_word(source, source_offset(i))) return source_offset(i);
        return source_bytes;
    }
    const LoopValues& loop_values() const noexcept { return loops_; }
    uint32_t booleans() const noexcept { return booleans_; }
    uint64_t version() const noexcept { return version_; }
private:
    static constexpr uint32_t source_offset(uint32_t word) {
        return word < 2 ? word * 16 : 32 + (word - 2) * 4;
    }
    static uint32_t read_word(std::span<const uint8_t, source_bytes> source, uint32_t offset) {
        uint32_t value;
        std::memcpy(&value, source.data() + offset, 4);
        return __builtin_bswap32(value);
    }
    std::array<uint32_t, 18> words_{};
    LoopValues loops_{};
    uint32_t booleans_ = 0;
    uint64_t version_ = 1;
    bool dirty_ = true;
};

// A version is valid only within one upload-ring lifetime. No array hashing,
// guest reads or reads from write-combined GPU storage are needed on a hit.
template <class Values>
class VersionedValueUpload {
public:
    struct Result { uint64_t offset; bool reused; };
    void reset() noexcept { owner_ = nullptr; }
    bool matches(const void* owner, uint64_t version) const noexcept {
        return owner && version && owner == owner_ && version == version_;
    }
    Result upload(const void* owner, uint64_t version, const Values& values,
                  uint64_t offset, uint8_t* destination) {
        if (!owner || !version) throw std::invalid_argument("constant upload needs a versioned owner");
        if (matches(owner, version)) return {offset_, true};
        std::memcpy(destination, values.data(), sizeof(values));
        owner_ = owner;
        version_ = version;
        offset_ = offset;
        return {offset, false};
    }
    // Opt-in direct guest-to-ring conversion. A cache hit touches neither source
    // nor destination and permits an empty source. A miss needs a full stage;
    // validate before writing or publishing the new allocation. This never
    // refreshes a GuestConstantState's independent sparse CPU shadow.
    Result upload_source(const void* owner, uint64_t version, std::span<const uint8_t> source,
                         uint64_t offset, uint8_t* destination) {
        static_assert(sizeof(Values) % 4 == 0);
        if (!owner || !version) throw std::invalid_argument("constant upload needs a versioned owner");
        if (matches(owner, version)) return {offset_, true};
        if (source.size() != sizeof(Values) || !source.data() || !destination)
            throw std::invalid_argument("direct constant upload needs a complete source and destination");
        swap_words_into({destination, sizeof(Values)}, source);
        owner_ = owner;
        version_ = version;
        offset_ = offset;
        return {offset, false};
    }
private:
    const void* owner_ = nullptr;
    uint64_t version_ = 0, offset_ = 0;
};
using VersionedConstantUpload = VersionedValueUpload<GuestConstantState::Values>;
using VersionedLoopConstantUpload = VersionedValueUpload<GuestShortConstantState::LoopValues>;
}
