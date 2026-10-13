#include "guest_buffer_state.h"
#include "guest_memory.h"
#include "native_formats.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iterator>

namespace sfr {
namespace {
constexpr uint64_t storage_budget = 128ull << 20;
constexpr size_t recipe_limit = 4;
std::atomic<uint64_t> next_version{0};
}
bool managed_buffers_enabled() {
    static const bool enabled = [] { const char* p = std::getenv("SFR_MANAGED_BUFFERS"); return p && *p != '0'; }();
    return enabled;
}
GuestBufferState::GuestBufferState(GuestMemory& memory) : memory_(memory) { memory.enable_write_versions(); }

bool GuestBufferState::describe(uint32_t resource, Description& result, uint32_t& common) {
    if (!resource || !memory_.readable(resource, 32)) return false;
    common = memory_.load<uint32_t>(resource);
    if ((common & 15) != 1) return false; // VB only; IB retains the existing cache.
    const uint32_t address = memory_.load<uint32_t>(uint64_t(resource) + 24) & ~3u;
    const uint32_t size = memory_.load<uint32_t>(uint64_t(resource) + 28) & 0x03fffffcu;
    result = {(address & 0x1fffffffu) + (address >= 0xe0000000u ? 0x1000u : 0u), size};
    return size && size <= (8u << 20);
}
void GuestBufferState::invalidate_recent() {
    // Clear before any borrowed location can be erased or moved. Wrap is safe
    // because no memo survives a structural change, including this one.
    recent_ = {};
    ++structure_;
}
uint64_t GuestBufferState::storage_bytes(const Publication& publication) {
    uint64_t bytes = publication.snapshot->words.size();
    for (const auto& prepared : publication.prepared) bytes += prepared.snapshot->words.size();
    return bytes;
}
void GuestBufferState::discard(Entry& entry) {
    invalidate_recent();
    for (const auto& [address, publication] : entry.ranges) bytes_ -= storage_bytes(publication);
    entry.ranges.clear();
    entry.pending.clear();
}
void GuestBufferState::discard_overlapping(Entry& entry, Description range) {
    invalidate_recent();
    auto it = entry.ranges.lower_bound(range.physical);
    if (it != entry.ranges.begin()) --it;
    while (it != entry.ranges.end() && uint64_t(it->first) < uint64_t(range.physical) + range.bytes) {
        if (uint64_t(it->first) + it->second.snapshot->words.size() > range.physical) {
            bytes_ -= storage_bytes(it->second);
            it = entry.ranges.erase(it);
        } else ++it;
    }
}
void GuestBufferState::created(uint32_t resource) {
    std::lock_guard guard(mutex_);
    invalidate_recent();
    if (auto old = entries_.find(resource); old != entries_.end()) { discard(old->second); entries_.erase(old); }
    Description description{};
    uint32_t common;
    if (describe(resource, description, common)) entries_.emplace(resource, Entry{description, ++generation_});
}
void GuestBufferState::lock(uint32_t resource) {
    std::lock_guard guard(mutex_);
    if (auto found = entries_.find(resource); found != entries_.end()) {
        discard(found->second);
        found->second.locked = true;
        found->second.range_failed = false;
    }
}
bool GuestBufferState::unchanged(const Publication& publication) {
    for (const auto& watch : publication.watches)
        if (!memory_.matches_write_versions(watch.address, publication.snapshot->words.size(), watch.versions)) return false;
    return !publication.watches.empty();
}
void GuestBufferState::publish(Entry& entry, Description range, bool transient) {
    discard_overlapping(entry, range);
    if (bytes_ + range.bytes > storage_budget) { ++counts_.rejected; return; }
    // Bound map/watch metadata as well as byte storage. Evicted CPU snapshots
    // only affect future lookups; queued draws already own immutable data.
    if (entry.ranges.size() >= 1024) {
        bytes_ -= storage_bytes(entry.ranges.begin()->second);
        entry.ranges.erase(entry.ranges.begin());
    }
    Publication publication;
    // Preserve protection for direct writes through every mapped alias.
    // Convert and watch the completed range, never the whole 8 MiB ring.
    for (uint64_t address : {uint64_t(range.physical) + 0xe0000000u - 0x1000u,
                            uint64_t(range.physical) | 0xa0000000u,
                            uint64_t(range.physical) | 0xc0000000u}) {
        if (address + range.bytes > 0x100000000ull || !memory_.readable(address, range.bytes)) continue;
        memory_.watch_writes(address, range.bytes, transient);
        auto versions = memory_.capture_write_versions(address, range.bytes);
        if (versions.empty()) { ++counts_.rejected; return; }
        publication.watches.push_back({uint32_t(address), std::move(versions)});
    }
    if (publication.watches.empty()) { ++counts_.rejected; return; }
    auto snapshot = std::make_shared<GuestBufferSnapshot>();
    snapshot->version = next_version.fetch_add(1, std::memory_order_relaxed) + 1;
    snapshot->generation = entry.generation;
    snapshot->physical = range.physical;
    snapshot->transient = transient;
    snapshot->words.resize(range.bytes);
    swap_words_into(snapshot->words, {memory_.base() + publication.watches.front().address, range.bytes});
    publication.snapshot = snapshot;
    if (!unchanged(publication)) { ++counts_.rejected; return; }
    bytes_ += range.bytes;
    counts_.converted_bytes += range.bytes;
    ++counts_.published;
    if (transient) { ++counts_.range_published; counts_.range_bytes += range.bytes; }
    // Once a static resource's layout is known, prepare it at Unlock. Dynamic
    // allocations can mix layouts, so prepare only the format actually bound.
    if (!transient)
        for (const auto& recipe : entry.recipes) prepare(entry, publication, recipe);
    entry.ranges.emplace(range.physical, std::move(publication));
}
std::shared_ptr<const GuestBufferSnapshot> GuestBufferState::prepare(
    Entry& entry, Publication& publication, const Recipe& recipe) {
    // Both budget fallback and prepared-vector growth retire the borrowed
    // recipe. A successful caller can remember the new stable element later.
    invalidate_recent();
    const auto& source = *publication.snapshot;
    const uint32_t start = (recipe.phase + recipe.stride -
        (source.physical - entry.description.physical) % recipe.stride) % recipe.stride;
    if (start >= source.words.size()) { ++counts_.rejected; return {}; }
    const uint64_t count = (source.words.size() - start) / recipe.stride;
    const uint32_t wide = recipe.stride + 8 * uint32_t(recipe.offsets.size());
    const uint64_t size = count * wide;
    // Transient versions are uploaded whole into the 64 MiB draw ring. Keep
    // their existing 8 MiB bound so indices/constants still fit; larger format
    // variants retain the legacy per-draw conversion path.
    if (!count || publication.prepared.size() >= recipe_limit || size > storage_budget - bytes_ ||
        (source.transient && size > (8ull << 20))) {
        ++counts_.rejected; return {};
    }
    auto result = std::make_shared<GuestBufferSnapshot>();
    result->version = next_version.fetch_add(1, std::memory_order_relaxed) + 1;
    result->generation = source.generation;
    result->physical = source.physical + start;
    result->transient = source.transient;
    result->words.resize(size);
    static const std::array<int16_t, 1024> snorm16 = [] {
        std::array<int16_t, 1024> table{};
        for (uint32_t bits = 0; bits < 1024; ++bits) {
            const int32_t value = int32_t(bits << 22) >> 22;
            table[bits] = int16_t(std::lround((std::max)(float(value) / 511.0f, -1.0f) * 32767.0f));
        }
        return table;
    }();
    for (uint64_t vertex = 0; vertex < count; ++vertex) {
        const uint8_t* from = source.words.data() + start + vertex * recipe.stride;
        uint8_t* to = result->words.data() + vertex * wide;
        std::memcpy(to, from, recipe.stride);
        for (size_t j = 0; j < recipe.offsets.size(); ++j) {
            uint32_t word;
            std::memcpy(&word, from + recipe.offsets[j], 4);
            const int16_t expanded[4] = {snorm16[word & 1023], snorm16[(word >> 10) & 1023],
                                        snorm16[(word >> 20) & 1023], 32767};
            std::memcpy(to + recipe.stride + 8 * j, expanded, 8);
        }
    }
    publication.prepared.push_back({recipe, result});
    bytes_ += size;
    ++counts_.prepared;
    counts_.prepared_bytes += size;
    return result;
}
void GuestBufferState::unlock(uint32_t resource, uint32_t previous_depth) {
    if (previous_depth != 0x100) return;
    std::lock_guard guard(mutex_);
    const auto found = entries_.find(resource);
    if (found == entries_.end()) return;
    auto& entry = found->second;
    if (!entry.locked) return;
    invalidate_recent();
    entry.locked = false;
    discard(entry);
    Description description{};
    uint32_t common;
    if (!describe(resource, description, common) || description != entry.description ||
        (common & 0xf00)) { ++counts_.rejected; return; }
    publish(entry, description, false);
}
void GuestBufferState::begin_range(uint32_t resource, uint32_t physical, uint32_t bytes) {
    std::lock_guard guard(mutex_);
    const auto found = entries_.find(resource);
    if (found == entries_.end()) return;
    auto& entry = found->second;
    invalidate_recent();
    if (!entry.locked) { entry.pending.clear(); entry.range_failed = false; }
    entry.locked = true;
    Description description{};
    uint32_t common;
    if (!describe(resource, description, common) || description != entry.description || !bytes ||
        ((physical | bytes) & 3) || physical < description.physical ||
        uint64_t(physical) - description.physical > description.bytes ||
        bytes > description.bytes - (physical - description.physical) || entry.pending.size() >= 64) {
        discard(entry); entry.range_failed = true; ++counts_.rejected; return;
    }
    discard_overlapping(entry, {physical, bytes});
    entry.pending.push_back({physical, bytes});
}
void GuestBufferState::finish_ranges(uint32_t resource, uint32_t previous_depth) {
    if (previous_depth != 0x100) return;
    std::lock_guard guard(mutex_);
    const auto found = entries_.find(resource);
    if (found == entries_.end()) return;
    auto& entry = found->second;
    if (!entry.locked) return;
    invalidate_recent();
    entry.locked = false;
    Description description{};
    uint32_t common;
    if (entry.range_failed || !describe(resource, description, common) || description != entry.description ||
        (common & 0xf00)) { discard(entry); ++counts_.rejected; return; }
    for (const auto range : entry.pending) publish(entry, range, true);
    entry.pending.clear();
}
uint64_t GuestBufferState::identity(uint32_t resource) {
    std::lock_guard guard(mutex_);
    const auto found = entries_.find(resource);
    return found == entries_.end() ? 0 : found->second.generation;
}
void GuestBufferState::released(uint32_t resource, uint64_t identity) {
    std::lock_guard guard(mutex_);
    const auto found = entries_.find(resource);
    if (found != entries_.end() && found->second.generation == identity) {
        discard(found->second); entries_.erase(found);
    }
}
std::shared_ptr<const GuestBufferSnapshot> GuestBufferState::lookup(uint32_t resource, uint32_t physical,
                                                                  uint64_t bytes, bool verify, const uint8_t* source) {
    std::lock_guard guard(mutex_);
    const auto selected = find_publication(resource, physical, bytes, verify, source);
    return selected.publication ? selected.publication->snapshot : nullptr;
}
GuestBufferState::Selection GuestBufferState::find_publication(uint32_t resource, uint32_t physical,
                                                              uint64_t bytes, bool verify, const uint8_t* source) {
    const bool recent_entry = recent_.entry && recent_.resource == resource && recent_.structure == structure_;
    Entry* selected_entry = recent_entry ? recent_.entry : nullptr;
    if (!selected_entry) {
        ++counts_.entry_lookups;
        const auto found = entries_.find(resource);
        if (found != entries_.end()) selected_entry = &found->second;
    }
    if (!selected_entry || selected_entry->ranges.empty() || selected_entry->locked) { ++counts_.missing; return {}; }
    auto& entry = *selected_entry;
    Description description{};
    uint32_t common;
    if (!describe(resource, description, common) || description != entry.description || (common & 0xf00)) {
        discard(entry); ++counts_.invalidated; return {};
    }
    if (!bytes || (physical & 3) || (bytes & 3) || physical < description.physical ||
        uint64_t(physical) - description.physical > description.bytes ||
        bytes > description.bytes - (physical - description.physical)) { ++counts_.rejected; return {}; }
    Publication* selected_publication = recent_entry ? recent_.publication : nullptr;
    if (selected_publication) {
        const auto& snapshot = selected_publication->snapshot;
        if (physical < snapshot->physical || uint64_t(physical) - snapshot->physical > snapshot->words.size() ||
            bytes > snapshot->words.size() - (physical - snapshot->physical)) selected_publication = nullptr;
    }
    if (!selected_publication) {
        ++counts_.range_lookups;
        auto range = entry.ranges.upper_bound(physical);
        if (range == entry.ranges.begin()) { ++counts_.missing; return {}; }
        selected_publication = &std::prev(range)->second;
    }
    auto& publication = *selected_publication;
    const auto& snapshot = publication.snapshot;
    const uint64_t offset = uint64_t(physical) - snapshot->physical;
    if (offset + bytes > snapshot->words.size()) { ++counts_.missing; return {}; }
    // A newly readable alias can change the draw's source without writing any
    // previously watched page. Reject that old publication before even the
    // verifier can compare it against its own obsolete source. No alias scan
    // or extra source read is needed on an unchanged draw.
    const bool source_changed = source && (publication.watches.empty() ||
        source != memory_.base() + publication.watches.front().address + offset);
    if (source_changed || !unchanged(publication)) {
        const auto range_physical = snapshot->physical;
        invalidate_recent();
        bytes_ -= storage_bytes(publication); entry.ranges.erase(range_physical); ++counts_.invalidated; return {};
    }
    if (verify) {
        const uint8_t* source = memory_.base() + publication.watches.front().address + offset;
        for (uint64_t i = 0; i < bytes; ++i)
            if (snapshot->words[offset + i] != source[i ^ 3])
                throw RuntimeStop("managed-buffer-mismatch", physical + i, "completed buffer snapshot differs from guest vertices");
    }
    if (!recent_entry || recent_.publication != &publication)
        recent_ = {resource, structure_, &entry, &publication, nullptr};
    ++counts_.hits;
    return {&entry, &publication, offset};
}
GuestVertexView GuestBufferState::vertices(uint32_t resource, uint32_t physical, uint64_t bytes, uint32_t stride,
                                          std::span<const uint32_t> dec3n_offsets, bool verify, const uint8_t* source) {
    std::lock_guard guard(mutex_);
    if (!stride || stride > 256 || (stride & 3) || bytes % stride || dec3n_offsets.size() > 16 ||
        std::any_of(dec3n_offsets.begin(), dec3n_offsets.end(),
                    [stride](uint32_t offset) { return offset > stride - 4; })) {
        ++counts_.rejected; return {};
    }
    const auto selected = find_publication(resource, physical, bytes, verify, source);
    auto* publication = selected.publication;
    if (!publication) return {};
    if (dec3n_offsets.empty()) return {publication->snapshot, selected.offset};
    auto& entry = *selected.entry;
    const uint32_t phase = (physical - entry.description.physical) % stride;
    // Compare borrowed declaration offsets before allocating a new recipe.
    const auto matches = [&](const Prepared& prepared) {
        return prepared.recipe.stride == stride && prepared.recipe.phase == phase &&
               std::ranges::equal(prepared.recipe.offsets, dec3n_offsets);
    };
    const Prepared* known = recent_.prepared;
    if (!known || !matches(*known)) {
        ++counts_.recipe_lookups;
        known = nullptr;
        for (const auto& prepared : publication->prepared)
            if (matches(prepared)) { known = &prepared; break; }
    }
    std::shared_ptr<const GuestBufferSnapshot> snapshot;
    if (known) { snapshot = known->snapshot; ++counts_.prepared_hits; }
    else {
        const Recipe recipe{stride, phase, {dec3n_offsets.begin(), dec3n_offsets.end()}};
        snapshot = prepare(entry, *publication, recipe);
        if (!snapshot) return {};
        known = &publication->prepared.back();
        // Learned recipes refer to the resource origin, never a draw's subrange.
        if (!snapshot->transient && entry.recipes.size() < recipe_limit &&
            std::find(entry.recipes.begin(), entry.recipes.end(), recipe) == entry.recipes.end())
            entry.recipes.push_back(recipe);
    }
    recent_ = {resource, structure_, &entry, publication, known};
    const uint64_t offset = uint64_t(physical - snapshot->physical) / stride * (stride + 8 * dec3n_offsets.size());
    if (verify) {
        const auto& raw = *publication->snapshot;
        const uint32_t wide = stride + 8 * uint32_t(dec3n_offsets.size());
        for (uint64_t vertex = 0; vertex < bytes / stride; ++vertex) {
            const auto* from = raw.words.data() + (physical - raw.physical) + vertex * stride;
            const auto* to = snapshot->words.data() + offset + vertex * wide;
            if (std::memcmp(from, to, stride))
                throw RuntimeStop("managed-buffer-format-mismatch", physical, "prepared vertex lost original attributes");
            for (size_t j = 0; j < dec3n_offsets.size(); ++j) {
                uint32_t word;
                std::memcpy(&word, from + dec3n_offsets[j], 4);
                int16_t actual[4];
                std::memcpy(actual, to + stride + 8 * j, 8);
                for (uint32_t component = 0; component < 4; ++component) {
                    const int32_t bits = (word >> (10 * (component % 3))) & 1023;
                    const int32_t signed_value = bits >= 512 ? bits - 1024 : bits;
                    const int16_t expected = component == 3 ? 32767 : int16_t(std::lround(
                        (std::max)(float(signed_value) / 511.0f, -1.0f) * 32767.0f));
                    if (actual[component] != expected)
                        throw RuntimeStop("managed-buffer-format-mismatch", physical, "prepared DEC3N differs from scalar conversion");
                }
            }
        }
    }
    return {std::move(snapshot), offset};
}
GuestBufferState::Counters GuestBufferState::counters() { std::lock_guard guard(mutex_); return counts_; }
}
