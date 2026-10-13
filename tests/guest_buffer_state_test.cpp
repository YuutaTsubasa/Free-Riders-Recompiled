#include "guest_buffer_state.h"
#include "guest_memory.h"
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

uint32_t word_at(const sfr::GuestBufferSnapshot& snapshot, size_t offset) {
    require(offset + 4 <= snapshot.words.size(), "test word read is in range");
    uint32_t result;
    std::memcpy(&result, snapshot.words.data() + offset, 4);
    return result;
}
int16_t lane_at(const sfr::GuestBufferSnapshot& snapshot, size_t offset) {
    require(offset + 2 <= snapshot.words.size(), "test lane read is in range");
    int16_t result;
    std::memcpy(&result, snapshot.words.data() + offset, 2);
    return result;
}
uint32_t packed(uint32_t x, uint32_t y, uint32_t z) {
    return (x & 1023) | ((y & 1023) << 10) | ((z & 1023) << 20);
}
int16_t expected_snorm(uint32_t bits) {
    // Integer reference, independent of the production floating-point lookup
    // table. Denominator 511 is odd, so exact half-way ties cannot occur.
    const int value = bits < 512 ? int(bits) : int(bits) - 1024;
    const int magnitude = value < 0 ? -value : value;
    const int normalized = magnitude >= 511 ? 32767 : (magnitude * 32767 + 255) / 511;
    return int16_t(value < 0 ? -normalized : normalized);
}
void require_dec3n(const sfr::GuestBufferSnapshot& snapshot, size_t offset, uint32_t source) {
    require(lane_at(snapshot, offset) == expected_snorm(source & 1023) &&
            lane_at(snapshot, offset + 2) == expected_snorm((source >> 10) & 1023) &&
            lane_at(snapshot, offset + 4) == expected_snorm((source >> 20) & 1023) &&
            lane_at(snapshot, offset + 6) == 32767, "prepared DEC3N matches independent signed-normalized reference");
}

template<uint32_t AddressOffset>
struct VertexFixture {
    static constexpr uint32_t header = 0x10000 + AddressOffset, backing = 0xe0010000 + AddressOffset,
                              physical = 0x11000 + AddressOffset;
    static constexpr uint32_t alias_a = physical | 0xa0000000, alias_c = physical | 0xc0000000;
    sfr::GuestMemory& memory;
    sfr::GuestBufferState state{memory};
    VertexFixture(sfr::GuestMemory& shared_memory, uint32_t bytes, uint32_t mapped_bytes) : memory(shared_memory) {
        memory.map(header, 4096);
        memory.map(backing, mapped_bytes);
        memory.map(alias_a, mapped_bytes);
        memory.map(alias_c, mapped_bytes);
        describe(header, bytes);
        state.created(header);
    }
    void describe(uint32_t resource, uint32_t bytes) {
        memory.store<uint32_t>(resource, 0x100001);
        memory.store<uint32_t>(resource + 24, backing | 3);
        memory.store<uint32_t>(resource + 28, bytes | 0x10000002);
    }
    void complete(uint32_t resource = header) {
        state.lock(resource);
        state.unlock(resource, 0x100);
    }
    void begin(uint32_t offset, uint32_t bytes) {
        memory.store<uint32_t>(header, 0x100101);
        state.begin_range(header, physical + offset, bytes);
    }
    void finish() {
        memory.store<uint32_t>(header, 0x100001);
        state.finish_ranges(header, 0x100);
    }
};

void prepared_static(sfr::GuestMemory& memory, bool direct) {
    // Four-byte prefix and suffix are deliberately not complete vertices.
    // Each component visits all 1024 encodings, including both negative
    // encodings that must clamp to -1 and the positive/negative endpoints.
    constexpr uint32_t stride = 12, count = 1024, wide = 28, bytes = count * stride + 8;
    using Fixture = VertexFixture<0x01000000>;
    constexpr auto header = Fixture::header, backing = Fixture::backing, physical = Fixture::physical;
    const std::array<uint32_t, 2> fields{0, 8}, reversed{8, 0}, short_fields{1, 4};
    const std::array<uint32_t, 1> one_field{0};
    Fixture fixture(memory, bytes, 16384);
    auto& state = fixture.state;
    memory.store<uint32_t>(backing, 0xdeadbeef);
    for (uint32_t i = 0; i < count; ++i) {
        memory.store<uint32_t>(backing + 4 + i * stride, packed(i, (i + 341) & 1023, (i + 683) & 1023));
        memory.store<uint32_t>(backing + 8 + i * stride, 0xc0000000 | i);
        memory.store<uint32_t>(backing + 12 + i * stride, packed(1023 - i, i ^ 512, (i * 31) & 1023));
    }
    memory.store<uint32_t>(backing + bytes - 4, 0xbaadf00d);
    require(!state.vertices(header, physical + 4, stride, stride, fields).snapshot,
            "unpublished data cannot acquire a prepared view");
    fixture.complete();
    auto raw = state.lookup(header, physical, bytes, true);
    const auto raw_view = state.vertices(header, physical + 4 + 17 * stride, 5 * stride, stride, {}, true);
    require(raw && raw_view.snapshot == raw && raw_view.offset == 4 + 17 * stride,
            "plain vertex view retains raw snapshot and physical byte offset");
    const auto first = state.vertices(header, physical + 4 + 17 * stride, 5 * stride, stride, fields, true);
    require(first.snapshot && first.snapshot != raw && !first.snapshot->transient &&
            first.snapshot->version != raw->version && first.snapshot->generation == raw->generation &&
            first.snapshot->physical == physical + 4 && first.snapshot->words.size() == count * wide &&
            first.offset == 17 * wide, "prepared view drops incomplete prefix/tail and translates the subrange offset");
    for (uint32_t i = 0; i < count; ++i) {
        const auto source_a = packed(i, (i + 341) & 1023, (i + 683) & 1023);
        const auto source_b = packed(1023 - i, i ^ 512, (i * 31) & 1023);
        require(word_at(*first.snapshot, i * wide) == source_a &&
                word_at(*first.snapshot, i * wide + 4) == (0xc0000000 | i) &&
                word_at(*first.snapshot, i * wide + 8) == source_b,
                "preparation preserves every original host-order vertex word");
        require_dec3n(*first.snapshot, i * wide + stride, source_a);
        require_dec3n(*first.snapshot, i * wide + stride + 8, source_b);
    }
    const auto after_first = state.counters();
    require(after_first.prepared == 1 && after_first.prepared_bytes == count * wide,
            "first format use converts complete records exactly once");
    for (uint32_t i = 0; i < 100; ++i) {
        const auto repeat = state.vertices(header, physical + 4 + i * stride, stride, stride, fields, true);
        require(repeat.snapshot == first.snapshot && repeat.offset == i * wide,
                "draws with different subranges reuse a prepared version");
    }
    require(state.counters().prepared == after_first.prepared &&
            state.counters().prepared_bytes == after_first.prepared_bytes &&
            state.counters().prepared_hits >= after_first.prepared_hits + 100,
            "repeated draws do not repeat DEC3N expansion");
    require(state.counters().entry_lookups == after_first.entry_lookups &&
            state.counters().range_lookups == after_first.range_lookups &&
            state.counters().recipe_lookups == after_first.recipe_lookups,
            "recent subranges reuse entry, publication and exact recipe locations");

    // Check rejection before filling the variant cap, so the cap cannot hide
    // an otherwise accepted invalid format or range.
    const std::array<uint32_t, 1> outside{12}, huge_offset{std::numeric_limits<uint32_t>::max()};
    require(!state.vertices(header, physical + 4, stride, 0, fields).snapshot &&
            !state.vertices(header, physical + 4, stride, 6, one_field).snapshot &&
            !state.vertices(header, physical + 4, 260, 260, one_field).snapshot,
            "invalid or unsupported vertex strides fall back");
    require(!state.vertices(header, physical + 4, stride, stride, outside).snapshot &&
            !state.vertices(header, physical + 4, stride, stride, huge_offset).snapshot,
            "out-of-stride and overflowing DEC3N fields fall back");
    require(!state.vertices(header, physical + 4, 0, stride, fields).snapshot &&
            !state.vertices(header, physical + 4, stride + 4, stride, fields).snapshot &&
            !state.vertices(header, physical + 5, stride, stride, fields).snapshot &&
            !state.vertices(header, physical + bytes - 4, stride, stride, fields).snapshot &&
            !state.vertices(header, physical + 4, std::numeric_limits<uint64_t>::max(), stride, fields).snapshot,
            "empty, fractional, unaligned and overflowing vertex spans fall back");

    const auto order_view = state.vertices(header, physical + 4, stride, stride, reversed, true);
    require(order_view.snapshot && order_view.snapshot != first.snapshot,
            "DEC3N field order is part of the format identity");
    require_dec3n(*order_view.snapshot, stride, word_at(*raw, 12));
    require_dec3n(*order_view.snapshot, stride + 8, word_at(*raw, 4));
    const auto before_return = state.counters();
    require(state.vertices(header, physical + 4, stride, stride, fields, true).snapshot == first.snapshot &&
            state.vertices(header, physical + 16, stride, stride, fields, true).snapshot == first.snapshot &&
            state.counters().prepared == before_return.prepared &&
            state.counters().entry_lookups == before_return.entry_lookups &&
            state.counters().range_lookups == before_return.range_lookups &&
            state.counters().recipe_lookups == before_return.recipe_lookups + 1,
            "A-B-A recipe order switches scan once, then memoize the original immutable variant");
    const auto phase_view = state.vertices(header, physical + 8, stride, stride, fields, true);
    require(phase_view.snapshot && phase_view.snapshot != first.snapshot &&
            phase_view.snapshot->physical == physical + 8 && phase_view.offset == 0,
            "record phase is part of the format identity");
    require_dec3n(*phase_view.snapshot, stride, word_at(*raw, 8));
    const auto stride_view = state.vertices(header, physical + 4, 16, 8, short_fields, true);
    require(stride_view.snapshot && stride_view.snapshot != first.snapshot &&
            stride_view.snapshot->physical == physical + 4 && stride_view.offset == 0,
            "record stride is part of the format identity");
    // Guest declarations may place a field across a host word boundary. The
    // legacy expansion reads it with memcpy, so that behavior must survive.
    require_dec3n(*stride_view.snapshot, 8, word_at(*raw, 5));
    require(state.counters().prepared == 4, "four independent format variants can be retained");
    require(!state.vertices(header, physical + 4, stride, stride, one_field).snapshot,
            "a fifth format variant falls back without evicting queued versions");
    require(state.vertices(header, physical + 4, stride, stride, fields, true).snapshot == first.snapshot &&
            state.vertices(header, physical, 4, 4, {}, true).snapshot == raw,
            "variant limit preserves existing format and raw bindings");

    const auto before_update = state.counters();
    const auto old_word = word_at(*first.snapshot, 0);
    state.lock(header);
    memory.store<uint32_t>(backing + 4, packed(0, 511, 512));
    memory.store<uint32_t>(header, 0x100101);
    state.unlock(header, 0x200);
    require(!state.vertices(header, physical + 4, stride, stride, fields).snapshot,
            "nested Unlock cannot expose a prepared version during a write");
    memory.store<uint32_t>(header, 0x100001);
    state.unlock(header, 0x100);
    require(state.counters().prepared == before_update.prepared + 4,
            "outer static Unlock prepares the bounded learned formats before any draw");
    const auto updated = state.vertices(header, physical + 4, stride, stride, fields, true);
    require(updated.snapshot && updated.snapshot->version != first.snapshot->version &&
            state.counters().prepared == before_update.prepared + 4,
            "first draw after static Unlock reuses its already-prepared version");
    require_dec3n(*updated.snapshot, stride, packed(0, 511, 512));
    require(word_at(*first.snapshot, 0) == old_word &&
            word_at(*updated.snapshot, wide + 4) == word_at(*first.snapshot, wide + 4),
            "partial source update preserves old queued data and unrelated records");

    const auto queued_words = updated.snapshot->words;
    std::thread writer([&] {
        // A page outside the current draw still belongs to the publication.
        if (direct) *reinterpret_cast<volatile uint8_t*>(memory.base() + Fixture::alias_a + 4100) = 0x7f;
        else memory.store<uint8_t>(Fixture::alias_a + 4100, 0x7f);
    });
    writer.join();
    memory.watch_writes(Fixture::alias_a, bytes);
    require(!state.vertices(header, physical + 4, stride, stride, fields).snapshot &&
            updated.snapshot->words == queued_words,
            "alias writes invalidate all future prepared bindings but preserve queued copies");

    const auto old_identity = state.identity(header);
    state.released(header, old_identity);
    require(!state.vertices(header, physical + 4, stride, stride, fields).snapshot,
            "released resources cannot serve prepared views");
    state.created(header);
    state.released(header, old_identity);
    const auto before_reuse = state.counters().prepared;
    fixture.complete();
    require(state.counters().prepared == before_reuse, "reused resource identity does not inherit old format recipes");
    const auto reused = state.vertices(header, physical + 4, stride, stride, fields, true);
    require(reused.snapshot && reused.snapshot->generation != first.snapshot->generation &&
            reused.snapshot->version != updated.snapshot->version,
            "reused header learns a new prepared version and ignores late release of old identity");
    state.released(header, old_identity);
    require(state.vertices(header, physical + 4, stride, stride, fields, true).snapshot == reused.snapshot,
            "late release cannot invalidate a warmed memo for the replacement identity");
    memory.store<uint32_t>(header + 28, 64 | 0x10000002);
    require(!state.vertices(header, physical + 4, stride, stride, fields).snapshot,
            "descriptor mutation invalidates prepared as well as raw versions");
}

void prepared_dynamic(sfr::GuestMemory& memory, bool direct) {
    using Fixture = VertexFixture<0x02000000>;
    constexpr auto header = Fixture::header, backing = Fixture::backing, physical = Fixture::physical;
    constexpr uint32_t stride = 12, wide = 20;
    const std::array<uint32_t, 1> fields{4};
    // Publishing this descriptor must never inspect its unmapped remainder.
    Fixture fixture(memory, 8u << 20, 8192);
    auto& state = fixture.state;
    fixture.begin(20, 44);
    for (uint32_t i = 0; i < 3; ++i) {
        memory.store<uint32_t>(backing + 24 + i * stride, 0xabcd0000 | i);
        memory.store<uint32_t>(backing + 28 + i * stride, packed(i, 511, 512));
        memory.store<uint32_t>(backing + 32 + i * stride, 0xbcde0000 | i);
    }
    require(!state.vertices(header, physical + 36, stride, stride, fields).snapshot,
            "an unfinished dynamic slice cannot acquire a prepared version");
    fixture.finish();
    require(state.counters().prepared == 0 && state.counters().range_bytes == 44,
            "dynamic finish publishes only its small raw allocation");
    const auto first = state.vertices(header, physical + 36, stride, stride, fields, true);
    require(first.snapshot && first.snapshot->transient && first.snapshot->physical == physical + 24 &&
            first.snapshot->words.size() == 3 * wide && first.offset == wide,
            "dynamic preparation aligns records relative to resource phase, not slice origin");
    for (uint32_t i = 0; i < 3; ++i) {
        require(word_at(*first.snapshot, i * wide) == (0xabcd0000 | i) &&
                word_at(*first.snapshot, i * wide + 8) == (0xbcde0000 | i),
                "dynamic preparation preserves the rest of each record");
        require_dec3n(*first.snapshot, i * wide + stride, packed(i, 511, 512));
    }
    const auto entire = state.vertices(header, physical + 24, 3 * stride, stride, fields, true);
    require(entire.snapshot == first.snapshot && entire.offset == 0 && state.counters().prepared == 1 &&
            state.counters().prepared_bytes == 3 * wide, "dynamic draws reuse one slice conversion");
    require(!state.vertices(header, physical + 24, 4 * stride, stride, fields).snapshot,
            "a draw cannot extend a prepared slice into unpublished bytes");

    const auto old_words = first.snapshot->words;
    fixture.begin(20, 44); // Wrap to the same guest ring allocation.
    memory.store<uint32_t>(backing + 28, packed(511, 512, 1023));
    require(!state.vertices(header, physical + 24, stride, stride, fields).snapshot,
            "ring reuse retires the old binding while the replacement is incomplete");
    fixture.finish();
    require(state.counters().prepared == 1, "dynamic wrap does not eagerly expand old learned formats");
    const auto wrapped = state.vertices(header, physical + 24, stride, stride, fields, true);
    require(wrapped.snapshot && wrapped.snapshot->version != first.snapshot->version &&
            first.snapshot->words == old_words && state.counters().prepared == 2,
            "dynamic wrap prepares a fresh immutable version on its first draw");
    require_dec3n(*wrapped.snapshot, stride, packed(511, 512, 1023));

    fixture.begin(4096, 44);
    memory.store<uint32_t>(backing + 4108, packed(512, 0, 511));
    fixture.finish();
    const auto separate = state.vertices(header, physical + 4104, 2 * stride, stride, fields, true);
    require(separate.snapshot && separate.snapshot->physical == physical + 4104 &&
            separate.snapshot->words.size() == 3 * wide &&
            state.vertices(header, physical + 24, stride, stride, fields, true).snapshot == wrapped.snapshot,
            "different slice origins respect the same resource-relative record phase");
    require_dec3n(*separate.snapshot, stride, packed(512, 0, 511));
    require(state.vertices(header, physical + 4104, stride, stride, fields, true).snapshot == separate.snapshot,
            "prime the dynamic publication memo before an alias write");
    if (direct) *reinterpret_cast<volatile uint8_t*>(memory.base() + Fixture::alias_c + 4108) = 0x21;
    else memory.store<uint8_t>(Fixture::alias_c + 4108, 0x21);
    memory.watch_writes(Fixture::alias_c + 4096, 44);
    require(!state.vertices(header, physical + 4104, stride, stride, fields).snapshot &&
            state.vertices(header, physical + 24, stride, stride, fields, true).snapshot == wrapped.snapshot,
            "dynamic alias invalidation removes only the affected publication");
    state.released(header, state.identity(header));
    require(!state.vertices(header, physical + 24, stride, stride, fields).snapshot &&
            first.snapshot->words == old_words, "dynamic release preserves already-queued converted data");
}

void prepared_budget(sfr::GuestMemory& memory) {
    using Fixture = VertexFixture<0x03000000>;
    constexpr auto header = Fixture::header, physical = Fixture::physical;
    constexpr uint32_t bytes = 8u << 20;
    const std::array<uint32_t, 1> fields{0};
    Fixture fixture(memory, bytes, bytes);
    auto& state = fixture.state;
    std::array<std::shared_ptr<const sfr::GuestBufferSnapshot>, 4> queued;
    // Each identity owns 8 MiB raw + 24 MiB prepared: four fill the common
    // 128 MiB cache budget exactly. All guest descriptors alias one mapping.
    for (uint32_t i = 0; i < 4; ++i) {
        const auto resource = header + i * 64;
        fixture.describe(resource, bytes);
        state.created(resource);
        fixture.complete(resource);
        queued[i] = state.vertices(resource, physical, 4, 4, fields, true).snapshot;
        require(queued[i] && queued[i]->words.size() == 3ull * bytes,
                "raw and prepared versions fit together up to the shared CPU budget");
    }
    require(state.vertices(header, physical, 4, 4, fields, true).snapshot == queued[0] &&
            state.vertices(header + 64, physical, 4, 4, fields, true).snapshot == queued[1] &&
            state.vertices(header, physical, 4, 4, fields, true).snapshot == queued[0],
            "resource A-B-A switches cannot reuse the other owner's memo despite identical physical ranges");
    const auto fifth = header + 4 * 64;
    fixture.describe(fifth, bytes);
    state.created(fifth);
    fixture.complete(fifth);
    require(!state.lookup(fifth, physical, 4) && !state.vertices(fifth, physical, 4, 4, fields).snapshot,
            "prepared storage counts toward the budget for later raw publications");
    require(state.vertices(header, physical, 4, 4, fields, true).snapshot == queued[0],
            "budget fallback preserves existing publications without a stale recent selection");
    state.released(header, state.identity(header));
    fixture.complete(fifth);
    const auto replacement = state.vertices(fifth, physical, 4, 4, fields, true);
    require(replacement.snapshot && replacement.snapshot->version != queued[0]->version &&
            lane_at(*queued[0], 4) == 0 && lane_at(*queued[0], 10) == 32767,
            "release returns raw plus prepared cache budget while queued data stays alive");
}

void prepared_dynamic_limit(sfr::GuestMemory& memory) {
    using Fixture = VertexFixture<0x04000000>;
    constexpr auto header = Fixture::header, physical = Fixture::physical;
    constexpr uint32_t bytes = 256u << 10;
    const std::array<uint32_t, 16> repeated_fields{};
    Fixture fixture(memory, bytes, bytes);
    fixture.begin(0, bytes);
    fixture.finish();
    auto& state = fixture.state;
    require(bool(state.lookup(header, physical, 4, true)), "large dynamic source can still use a raw binding");
    // A tiny draw must not cause an 8.25 MiB expanded publication to be
    // uploaded whole into the shared ring. The legacy draw path can handle
    // this span without preparing the rest of the publication.
    require(!state.vertices(header, physical, 4, 4, repeated_fields, true).snapshot &&
            state.counters().prepared == 0 && state.counters().prepared_bytes == 0 &&
            bool(state.lookup(header, physical, 4, true)),
            "oversized dynamic format falls back without removing the valid raw publication");
}

void recent_header_and_verify(sfr::GuestMemory& memory) {
    using Fixture = VertexFixture<0x05000000>;
    constexpr auto header = Fixture::header, backing = Fixture::backing, physical = Fixture::physical;
    Fixture fixture(memory, 64, 4096);
    auto& state = fixture.state;
    fixture.complete();
    auto first = state.lookup(header, physical, 4, true);
    const auto before_common = state.counters();
    memory.store<uint32_t>(header, 0x200001); // Refcount bits are not descriptor identity.
    require(first && state.lookup(header, physical, 4, true) == first &&
            state.counters().entry_lookups == before_common.entry_lookups &&
            state.counters().range_lookups == before_common.range_lookups,
            "a memo hit rereads common without treating unrelated common bits as a new descriptor");
    memory.store<uint32_t>(header, 0x100101); // A writer missing the Lock hook.
    require(!state.lookup(header, physical, 4), "memo hits still reject a changed lock depth in the guest header");
    fixture.describe(header, 64);
    require(!state.lookup(header, physical, 4), "restoring the header cannot revive a discarded publication");
    fixture.complete();
    require(bool(state.lookup(header, physical, 4)), "republication primes the header guard memo");
    memory.store<uint32_t>(header, 0x100002);
    require(!state.lookup(header, physical, 4), "memo hits still reject a changed resource type");
    fixture.describe(header, 64);
    fixture.complete();
    require(bool(state.lookup(header, physical, 4)), "republication primes the address guard memo");
    memory.store<uint32_t>(header + 24, (backing + 4096) | 3);
    require(!state.lookup(header, physical, 4), "memo hits still reject a changed backing address");
    fixture.describe(header, 64);
    fixture.complete();
    const std::array<uint32_t, 1> fields{0};
    const auto old_identity = state.identity(header);
    std::weak_ptr<const sfr::GuestBufferSnapshot> retired = state.vertices(header, physical, 4, 4, fields).snapshot;
    require(!retired.expired(), "replacement test has a live prepared publication");
    state.created(header); // Same guest header, without a matching old Release.
    require(retired.expired() && !state.lookup(header, physical, 4),
            "Create retires borrowed recipe locations and does not retain old snapshot ownership");
    fixture.complete();
    const auto replacement = state.vertices(header, physical, 4, 4, fields, true);
    state.released(header, old_identity);
    require(replacement.snapshot && replacement.snapshot->generation != old_identity &&
            state.vertices(header, physical, 4, 4, fields, true).snapshot == replacement.snapshot,
            "a warmed replacement memo survives a delayed old Release");

    // Deliberately damage host snapshots to prove memo hits still invoke the
    // independent verifier. Guest bytes and write versions remain unchanged.
    auto raw = std::const_pointer_cast<sfr::GuestBufferSnapshot>(state.lookup(header, physical, 4));
    raw->words[0] ^= 1;
    bool raw_mismatch = false;
    try { state.lookup(header, physical, 4, true); }
    catch (const sfr::RuntimeStop& e) { raw_mismatch = e.category == "managed-buffer-mismatch"; }
    raw->words[0] ^= 1;
    require(raw_mismatch, "raw verification is not bypassed by the recent publication memo");
    auto prepared = std::const_pointer_cast<sfr::GuestBufferSnapshot>(replacement.snapshot);
    prepared->words[4] ^= 1;
    bool prepared_mismatch = false;
    try { state.vertices(header, physical, 4, 4, fields, true); }
    catch (const sfr::RuntimeStop& e) { prepared_mismatch = e.category == "managed-buffer-format-mismatch"; }
    prepared->words[4] ^= 1;
    require(prepared_mismatch, "format verification is not bypassed by the recent recipe memo");
}

void recent_range_eviction(sfr::GuestMemory& memory) {
    using Fixture = VertexFixture<0x06000000>;
    constexpr auto header = Fixture::header, physical = Fixture::physical;
    Fixture fixture(memory, 8192, 8192);
    auto& state = fixture.state;
    // Write no source pages while publishing adjacent slices: all 1024 watch
    // sets should remain valid, independently of which slice is most recent.
    for (uint32_t i = 0; i < 1024; ++i) {
        fixture.begin(i * 4, 4);
        fixture.finish();
    }
    const std::array<uint32_t, 1> fields{0};
    std::weak_ptr<const sfr::GuestBufferSnapshot> first_raw = state.lookup(header, physical, 4, true);
    std::weak_ptr<const sfr::GuestBufferSnapshot> first_prepared = state.vertices(header, physical, 4, 4, fields, true).snapshot;
    require(!first_raw.expired() && !first_prepared.expired(), "range-cap test warms both borrowed locations");
    fixture.begin(4096, 4);
    fixture.finish();
    require(first_raw.expired() && first_prepared.expired() && !state.lookup(header, physical, 4),
            "the 1025th range evicts the warmed first range without memo snapshot retention");
    require(state.lookup(header, physical + 4, 4, true) && state.lookup(header, physical + 4096, 4, true),
            "range eviction preserves the remaining ranges and the new publication");

    std::weak_ptr<const sfr::GuestBufferSnapshot> overlap = state.vertices(header, physical + 8, 4, 4, fields, true).snapshot;
    fixture.begin(8, 8); // Replace two adjacent ranges while the first is recent.
    require(overlap.expired() && !state.lookup(header, physical + 8, 4),
            "overlap retirement clears the borrowed prepared location before the replacement is complete");
    fixture.finish();
    const auto joined = state.lookup(header, physical + 12, 4, true);
    require(joined && joined->physical == physical + 8 && joined->words.size() == 8,
            "lookup after overlap selects the new containing publication");
    std::weak_ptr<const sfr::GuestBufferSnapshot> released = state.vertices(header, physical + 12, 4, 4, fields, true).snapshot;
    state.released(header, state.identity(header));
    require(released.expired() && !state.vertices(header, physical + 12, 4, 4, fields).snapshot,
            "Release clears a warmed recipe without extending its snapshot lifetime");
}

void run(sfr::GuestMemory& memory, bool direct) {
    sfr::GuestBufferState state(memory);
    constexpr uint32_t header = 0x10000, backing = 0xe0010000, physical = 0x11000, size = 64;
    memory.map(header, 4096);
    memory.map(backing, 4096);
    memory.map(0xa0011000, 4096);
    memory.map(0xc0011000, 4096);
    const auto describe = [&] {
        memory.store<uint32_t>(header, 0x100001);
        memory.store<uint32_t>(header + 24, backing | 3);
        memory.store<uint32_t>(header + 28, size | 0x10000002);
    };
    describe();
    state.created(header);
    require(!state.lookup(header, physical, size), "created without Unlock must fall back");
    state.lock(header);
    for (uint32_t i = 0; i < size; i += 4) memory.store<uint32_t>(backing + i, 0x11223300 + i);
    memory.store<uint32_t>(header, 0x100101);
    state.unlock(header, 0x200);
    require(!state.lookup(header, physical, size), "nested Unlock must not publish");
    memory.store<uint32_t>(header, 0x100001);
    state.unlock(header, 0x100);
    auto first = state.lookup(header, physical, size, true);
    require(bool(first), "outer Unlock publishes a snapshot immediately, even in the same frame");
    uint32_t word = 0;
    std::memcpy(&word, first->words.data() + 4, 4);
    require(word == 0x11223304, "Unlock converts to native word order");
    const auto before_repeats = state.counters();
    for (int i = 0; i < 100; ++i)
        require(state.lookup(header, physical + 16, 32, true) == first, "draws and subranges reuse one version");
    require(state.counters().published == 1 && state.counters().converted_bytes == size, "no per-draw conversion");
    require(state.counters().entry_lookups == before_repeats.entry_lookups &&
            state.counters().range_lookups == before_repeats.range_lookups,
            "raw subrange hits avoid repeated hash and range-map searches");
    require(!state.lookup(header, physical + size - 4, 8), "out-of-bounds slice falls back");
    require(!state.lookup(header, physical + 1, 4), "unaligned slice falls back");

    // Any alias can write, and another cache can rearm before we next draw.
    const auto write = [&](uint32_t address, uint8_t value) {
        if (direct) *reinterpret_cast<volatile uint8_t*>(memory.base() + address) = value;
        else memory.store<uint8_t>(address, value);
    };
    std::thread writer([&] { write(0xa0011000 + 4, 0x99); });
    writer.join();
    memory.watch_writes(0xa0011000, size);
    require(!state.lookup(header, physical, size), "alias write invalidates even after same-frame rearm");
    require(first->words[4] == 4, "queued snapshot stays unchanged after a guest write");

    state.lock(header);
    memory.store<uint32_t>(backing + 4, 0xaabbccdd);
    state.unlock(header, 0x100);
    auto second = state.lookup(header, physical, size, true);
    require(bool(second) && second->version != first->version, "partial update publishes a new immutable version");
    require(second->words[0] == first->words[0] && second->words[4] == 0xdd, "partial update preserves unaffected bytes");

    const uint64_t old_identity = state.identity(header);
    state.released(header, old_identity);
    require(!state.lookup(header, physical, size), "release removes future bindings");
    describe();
    state.created(header);
    state.released(header, old_identity);
    require(state.identity(header) != 0 && state.identity(header) != old_identity, "late release cannot remove reused header");
    state.lock(header);
    state.unlock(header, 0x100);
    auto reused = state.lookup(header, physical, size, true);
    require(bool(reused) && reused->generation != second->generation, "reused resource has a distinct generation");
    memory.store<uint32_t>(header + 28, 32 | 0x10000002);
    require(!state.lookup(header, physical, 16), "descriptor mutation invalidates the resource");

    describe(); state.created(header); state.lock(header); state.unlock(header, 0x100);
    require(bool(state.lookup(header, physical, size)), "fresh publication before backing replacement");
    memory.decommit(backing, 4096);
    memory.commit(backing, 4096);
    require(!state.lookup(header, physical, size), "backing replacement invalidates without a Lock");
    require(first->words[4] == 4 && second->words[4] == 0xdd, "old versions survive release and backing reuse");

    // Only two pages are mapped despite an 8 MiB descriptor. Publishing a
    // small ring allocation must never read or convert the unmapped remainder.
    memory.map(backing + 4096, 4096);
    memory.map(0xa0011000 + 4096, 4096);
    memory.map(0xc0011000 + 4096, 4096);
    describe();
    memory.store<uint32_t>(header + 28, (8u << 20) | 0x10000002);
    state.created(header);
    const auto start_range = [&](uint32_t offset, uint32_t bytes, uint32_t depth = 0x100) {
        memory.store<uint32_t>(header, 0x100001 | depth);
        state.begin_range(header, physical + offset, bytes);
    };
    const auto finish_range = [&](uint32_t depth = 0x100) {
        memory.store<uint32_t>(header, 0x100001 | (depth - 0x100));
        state.finish_ranges(header, depth);
    };
    const auto before_range = state.counters().converted_bytes;
    start_range(16, 32);
    memory.store<uint32_t>(backing + 16, 0x11223344);
    require(!state.lookup(header, physical + 16, 32), "unfinished ring writes cannot be drawn through a snapshot");
    finish_range();
    auto slice = state.lookup(header, physical + 16, 32, true);
    require(slice && slice->transient && slice->physical == physical + 16 && slice->words.size() == 32,
            "dynamic publication represents exactly the completed range");
    require(state.counters().converted_bytes - before_range == 32, "small ring update does not convert all 8 MiB");
    require(!state.lookup(header, physical, 64), "draw spanning unpublished bytes falls back");
    start_range(4096, 16);
    memory.store<uint32_t>(backing + 4096, 0xaabbccdd);
    finish_range();
    auto separate = state.lookup(header, physical + 4096, 16, true);
    require(separate && state.lookup(header, physical + 16, 32, true) == slice,
            "nonoverlapping completed allocations on different pages remain available");
    start_range(16, 16); // Ring wrap/reuse at an earlier offset.
    memory.store<uint32_t>(backing + 16, 0x55667788);
    finish_range();
    auto wrapped = state.lookup(header, physical + 16, 16, true);
    require(wrapped && wrapped->version != slice->version && slice->words[0] == 0x44 && wrapped->words[0] == 0x88,
            "ring wrap publishes a fresh immutable version without changing queued data");
    require(!state.lookup(header, physical + 16, 32), "partial overlap retires the entire older published range");
    require(state.lookup(header, physical + 4096, 16, true) == separate, "wrap preserves unrelated allocations");
    start_range(128, 16);
    memory.store<uint32_t>(backing + 128, 0x12345678);
    start_range(256, 16, 0x200);
    memory.store<uint32_t>(backing + 256, 0x90abcdef);
    finish_range(0x200);
    require(!state.lookup(header, physical + 128, 16), "inner ring finish does not publish pending allocations");
    finish_range();
    require(state.lookup(header, physical + 128, 16, true) && state.lookup(header, physical + 256, 16, true),
            "outer ring finish publishes all nested completed ranges");
    write(0xa0011000 + 4096, 0x77);
    memory.watch_writes(0xa0011000 + 4096, 16);
    require(!state.lookup(header, physical + 4096, 16), "dynamic ranges also detect alias writes after rearm");
    start_range((8u << 20) - 4, 8);
    finish_range();
    require(!state.lookup(header, physical + 128, 16), "invalid ring allocation cannot preserve stale publications");
}
}
// The actual draw selects the newly readable preferred E alias. A retained
// publication from the A alias must not silently supply different bytes.
static void source_selection_regression(sfr::GuestMemory& memory) {
    for (uint32_t mode = 0; mode < 4; ++mode) {
        const bool dynamic = (mode & 2) != 0, prepared = (mode & 1) != 0;
        const uint32_t delta = mode * 0x00100000;
        const uint32_t header = 0x07010000 + delta, physical = 0x07011000 + delta;
        const uint32_t alias_a = 0xa7011000 + delta, alias_e = 0xe7010000 + delta;
        const uint32_t range = dynamic ? 32 : 0, offset = range + 8;
        const auto old_word = packed(1, 2, 3), new_word = packed(250, 4, 2);
        const std::array<uint32_t, 1> field{0};
        const auto fields = prepared ? std::span<const uint32_t>(field) : std::span<const uint32_t>{};
        memory.map(header, 4096);
        memory.map(alias_a, 8192);
        memory.store<uint32_t>(header, 0x100001);
        memory.store<uint32_t>(header + 24, alias_e | 3);
        memory.store<uint32_t>(header + 28, (dynamic ? 8192 : 64) | 0x10000002);
        memory.store<uint32_t>(alias_a + offset, old_word);
        memory.store<uint32_t>(alias_a + 4096, 0x12345678);
        sfr::GuestBufferState state(memory);
        state.created(header);
        const auto complete = [&](uint32_t begin = 32, uint32_t bytes = 32) {
            if (!dynamic) { state.lock(header); state.unlock(header, 0x100); return; }
            memory.store<uint32_t>(header, 0x100101);
            state.begin_range(header, physical + begin, bytes);
            memory.store<uint32_t>(header, 0x100001);
            state.finish_ranges(header, 0x100);
        };
        complete();
        std::shared_ptr<const sfr::GuestBufferSnapshot> separate;
        if (dynamic) {
            complete(4096, 4);
            separate = state.lookup(header, physical + 4096, 4, true, memory.base() + alias_a + 4096);
            require(bool(separate), "separate dynamic publication remains on the A-only page");
        }
        const auto old = state.vertices(header, physical + offset, 4, 4, fields, true,
                                        memory.base() + alias_a + offset);
        require(old.snapshot && word_at(*old.snapshot, old.offset) == old_word,
                "A-only managed source is published with its correct subrange offset");
        if (prepared) require_dec3n(*old.snapshot, old.offset + 4, old_word);
        require(state.vertices(header, physical + offset, 4, 4, fields, true,
                               memory.base() + alias_a + offset).snapshot == old.snapshot,
                "unchanged caller source can reuse the recent raw or prepared publication");
        memory.map(alias_e, 4096);
        memory.store<uint32_t>(alias_e + offset, new_word);
        // Neither A nor its descriptor was written. The earlier verifier would
        // compare against old A too, so verification alone could not catch this.
        const auto next = state.vertices(header, physical + offset, 4, 4, fields, true,
                                         memory.base() + alias_e + offset);
        require(!next.snapshot, "newly selected E source must retire the old managed A publication");
        require(word_at(*old.snapshot, old.offset) == old_word,
                "source changes preserve immutable snapshots held by already queued draws");
        if (prepared) require_dec3n(*old.snapshot, old.offset + 4, old_word);
        if (dynamic)
            require(state.lookup(header, physical + 4096, 4, true, memory.base() + alias_a + 4096) == separate,
                    "source mismatch retires only its publication, not unrelated dynamic slices");
        complete();
        const auto replacement = state.vertices(header, physical + offset, 4, 4, fields, true,
                                                memory.base() + alias_e + offset);
        require(replacement.snapshot && replacement.snapshot->version != old.snapshot->version &&
                word_at(*replacement.snapshot, replacement.offset) == new_word,
                "a completed update republishes from the new preferred E source");
        if (prepared) require_dec3n(*replacement.snapshot, replacement.offset + 4, new_word);
        require(!state.lookup(header, physical + offset, 4, true, memory.base() + alias_e + offset + 4),
                "the supplied source must match the exact drawn subrange, not only the alias");
    }
}
int main(int argc, char**) {
    try {
        const bool direct = argc > 1;
        sfr::GuestMemory::direct_guest_access = direct;
        // The process-wide fault handler watches one GuestMemory for its
        // lifetime. Independent fixtures share it but use disjoint addresses
        // so protected pages and fault streaks cannot leak between scenarios.
        sfr::GuestMemory memory;
        run(memory, direct);
        prepared_static(memory, direct);
        prepared_dynamic(memory, direct);
        prepared_budget(memory);
        prepared_dynamic_limit(memory);
        recent_header_and_verify(memory);
        recent_range_eviction(memory);
        source_selection_regression(memory);
        std::cout << "Managed buffer lifecycle and prepared vertex checks passed\n";
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
