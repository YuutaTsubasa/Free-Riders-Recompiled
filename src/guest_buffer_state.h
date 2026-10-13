#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <map>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace sfr {
class GuestMemory;
bool managed_buffers_enabled();

// Raw word conversion happens at outer Unlock or the ring-write finish.
// Format variants are prepared once on first use, and at later static Unlocks.
// Static versions own a host buffer; dynamic versions share the draw ring.
// Both are immutable while queued commands or the GPU can still read them.
struct GuestBufferSnapshot {
    uint64_t version = 0, generation = 0;
    uint32_t physical = 0;
    bool transient = false; // Dynamic slice uploaded through the frame arena.
    std::vector<uint8_t> words;
};

// Offset is in the host layout, which can be wider than the guest stride.
struct GuestVertexView {
    std::shared_ptr<const GuestBufferSnapshot> snapshot;
    uint64_t offset = 0;
};

class GuestBufferState {
public:
    // Construct before parallel guest execution (initializes precise watches).
    explicit GuestBufferState(GuestMemory& memory);
    void created(uint32_t resource);
    void lock(uint32_t resource);
    void unlock(uint32_t resource, uint32_t previous_depth);
    void begin_range(uint32_t resource, uint32_t physical, uint32_t bytes);
    void finish_ranges(uint32_t resource, uint32_t previous_depth);
    uint64_t identity(uint32_t resource);
    void released(uint32_t resource, uint64_t identity);
    // Draw callers supply their selected guest source, including slice offset.
    // An omitted source inspects the publication's own backing; it must not be
    // used to bind a draw that selected an independently mapped physical alias.
    std::shared_ptr<const GuestBufferSnapshot> lookup(uint32_t resource, uint32_t physical, uint64_t bytes,
                                                     bool verify = false, const uint8_t* source = nullptr);
    GuestVertexView vertices(uint32_t resource, uint32_t physical, uint64_t bytes, uint32_t stride,
                             std::span<const uint32_t> dec3n_offsets, bool verify = false,
                             const uint8_t* source = nullptr);
    struct Counters {
        uint64_t published = 0, converted_bytes = 0, hits = 0, missing = 0, invalidated = 0, rejected = 0;
        uint64_t range_published = 0, range_bytes = 0;
        uint64_t prepared = 0, prepared_bytes = 0, prepared_hits = 0;
        // Actual resolver searches only; recent hits do not increment these.
        uint64_t entry_lookups = 0, range_lookups = 0, recipe_lookups = 0;
    };
    Counters counters();
private:
    struct Description {
        uint32_t physical, bytes;
        bool operator==(const Description&) const = default;
    };
    struct Watch { uint32_t address; std::vector<uint64_t> versions; };
    struct Recipe {
        uint32_t stride, phase; // Record origin relative to the resource base.
        std::vector<uint32_t> offsets;
        bool operator==(const Recipe&) const = default;
    };
    struct Prepared {
        Recipe recipe;
        std::shared_ptr<const GuestBufferSnapshot> snapshot;
    };
    struct Publication {
        std::shared_ptr<const GuestBufferSnapshot> snapshot;
        std::vector<Watch> watches;
        std::vector<Prepared> prepared;
    };
    struct Entry {
        Description description;
        uint64_t generation;
        bool locked = false;
        std::map<uint32_t, Publication> ranges;
        std::vector<Description> pending;
        bool range_failed = false;
        std::vector<Recipe> recipes; // Bounded, learned static layouts survive Lock.
    };
    struct Selection {
        Entry* entry = nullptr;
        Publication* publication = nullptr;
        uint64_t offset = 0;
    };
    struct Recent {
        uint32_t resource = 0;
        uint64_t structure = 0;
        Entry* entry = nullptr;
        Publication* publication = nullptr;
        const Prepared* prepared = nullptr;
    };
    bool describe(uint32_t resource, Description& result, uint32_t& common);
    void invalidate_recent();
    void discard(Entry& entry);
    bool unchanged(const Publication& publication);
    void discard_overlapping(Entry& entry, Description range);
    void publish(Entry& entry, Description range, bool transient);
    static uint64_t storage_bytes(const Publication& publication);
    Selection find_publication(uint32_t resource, uint32_t physical, uint64_t bytes, bool verify,
                               const uint8_t* source);
    std::shared_ptr<const GuestBufferSnapshot> prepare(Entry& entry, Publication& publication, const Recipe& recipe);
    GuestMemory& memory_;
    std::mutex mutex_;
    std::unordered_map<uint32_t, Entry> entries_;
    uint64_t generation_ = 0, bytes_ = 0;
    // Borrowed locations only, under mutex_. This never keeps snapshots alive
    // and never substitutes for the descriptor or full publication guards.
    uint64_t structure_ = 1;
    Recent recent_;
    Counters counts_;
};
}
