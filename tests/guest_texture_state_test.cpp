#include "guest_texture_state.h"
#include <iostream>

namespace {
using State = sfr::GuestTextureState;
using Source = std::array<uint8_t, State::source_bytes>;
static_assert(State::sampled_slots == 16 && State::source_bytes == 384);
static_assert(State::dirty_mask == 0xFFFF0000ull);

void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}
void store_word(Source& source, uint32_t slot, uint32_t word, uint32_t value) {
    const uint32_t offset = (slot * 6 + word) * 4;
    for (uint32_t byte = 0; byte < 4; ++byte)
        source[offset + byte] = uint8_t(value >> (24 - byte * 8));
}
uint32_t initial_word(uint32_t slot, uint32_t word) {
    return 0x80A1C2E3u ^ (slot << 24) ^ (word << 8);
}
Source source_values() {
    Source source{};
    for (uint32_t slot = 0; slot < 16; ++slot)
        for (uint32_t word = 0; word < 6; ++word)
            store_word(source, slot, word, initial_word(slot, word));
    return source;
}

void initial_snapshot_and_clean_draws() {
    State state;
    Source source = source_values();
    require(state.pending_mask(0) == 0xFFFF0000ull, "first use invalidates every sampled slot even with clean guest state");
    require(state.prepare(0, source) == 384, "first use reads the complete sampled fetch region");
    for (uint32_t slot = 0; slot < 16; ++slot) {
        require(state.slot(slot).version == 1, "first publication assigns an independent nonzero slot version");
        for (uint32_t word = 0; word < 6; ++word)
            require(state.slot(slot).words[word] == initial_word(slot, word), "every fetch word is decoded big endian");
    }
    require(state.pending_mask(0) == 0 && state.first_mismatch(source) == 96,
            "successful first publication is clean and agrees with guest words");
    store_word(source, 0, 0, 0x01234567);
    store_word(source, 15, 5, 0xFEDCBA98);
    require(state.prepare(0, source) == 0 && state.slot(0).words[0] == initial_word(0, 0) &&
            state.slot(15).words[5] == initial_word(15, 5) && state.slot(0).version == 1,
            "clean preparation reads nothing and does not adopt an untracked source change");
    require(state.first_mismatch(source) == 0, "verification detects an untracked first word");
    store_word(source, 0, 0, initial_word(0, 0));
    require(state.first_mismatch(source) == 95, "verification covers the last word of the last sampled slot");

    State zero_state;
    Source zeros{};
    require(zero_state.prepare(0, zeros) == 384 && zero_state.slot(0).version == 1 &&
            zero_state.slot(15).version == 1, "an initially all-zero fetch still receives a published version");
}

void dirty_slots_and_unconsumed_bits() {
    State state;
    Source source = source_values();
    state.prepare(0, source);
    constexpr uint64_t unsampled = 0xFFFFFFFF0000FFFFull;
    require(state.pending_mask(unsampled) == 0 && state.prepare(unsampled, source) == 0,
            "upper32, low6 and texture slots16..25 are not consumed by the sixteen-slot cache");
    store_word(source, 0, 2, 0x12345678);
    store_word(source, 15, 3, 0x87654321);
    store_word(source, 7, 4, 0x01020304);
    require(state.pending_mask(unsampled | 0x80010000ull) == 0x80010000ull,
            "slot0 and slot15 map to bits31 and16 without including unrelated guest state");
    require(state.prepare(unsampled | 0x80010000ull, source) == 48,
            "two disjoint dirty slots read exactly two complete fetch records");
    require(state.slot(0).words[2] == 0x12345678 && state.slot(15).words[3] == 0x87654321 &&
            state.slot(0).version == 2 && state.slot(15).version == 2,
            "endpoint dirty slots receive their own changed values and versions");
    require(state.slot(7).words[4] == initial_word(7, 4) && state.slot(7).version == 1 &&
            state.first_mismatch(source) == 46,
            "dirty refresh leaves an unflagged slot unchanged and visible to verification");
    require(state.prepare(0x01000000ull, source) == 24 && state.slot(7).words[4] == 0x01020304 &&
            state.slot(7).version == 2 && state.first_mismatch(source) == 96,
            "an inline game writer can publish through the original slot7 dirty bit alone");
    require(state.prepare(0x80010000ull, source) == 48 && state.slot(0).version == 2 && state.slot(15).version == 2,
            "same-value setters consume their dirty bits without changing descriptor versions");
    require(state.prepare(0, source) == 0, "prepared guest bits are not retained as host pending work");

    // Check every mapping, not just endpoints, and every word within each record.
    for (uint32_t slot = 0; slot < 16; ++slot) {
        const auto version = state.slot(slot).version;
        for (uint32_t word = 0; word < 6; ++word) {
            const uint32_t value = 0x31415926u ^ (slot << 12) ^ word;
            store_word(source, slot, word, value);
            require(state.prepare(0x80000000ull >> slot, source) == 24 && state.slot(slot).words[word] == value &&
                    state.slot(slot).version == version + word + 1,
                    "every original slot dirty bit refreshes all six host-decoded words");
        }
    }
    require(state.first_mismatch(source) == 96, "all slot mappings preserve a complete correct snapshot");
}

void explicit_lifecycle_invalidation() {
    State state;
    Source source = source_values();
    store_word(source, 3, 0, 0x80000802);
    store_word(source, 3, 1, 0x00123054);
    state.prepare(0, source);
    const auto bound = state.slot(3).words;
    store_word(source, 3, 0, bound[0] & ~3u); // SetTexture(NULL) leaves the other words and guest dirty untouched.
    require(state.prepare(0, source) == 0 && state.first_mismatch(source) == 18,
            "NULL cannot be detected through the guest dirty word alone");
    state.invalidate_slot(3);
    state.invalidate_slot(3);
    require(state.pending_mask(0) == 0x10000000ull && state.prepare(0, source) == 24,
            "repeated external NULL invalidations collapse into one slot refresh");
    require(state.slot(3).words[0] == 0x80000800 && state.slot(3).version == 2,
            "NULL changes the published type bits without forcing the whole fetch to zero");
    for (uint32_t word = 1; word < 6; ++word)
        require(state.slot(3).words[word] == bound[word], "NULL preserves the retained base and sampler fields");

    state.invalidate_slot(0);
    store_word(source, 0, 5, 0xABCDEF01);
    store_word(source, 15, 5, 0x12345678);
    require(state.pending_mask(0x00010000ull) == 0x80010000ull && state.prepare(0x00010000ull, source) == 48,
            "host events and guest dirty bits merge into one refresh without losing either");
    for (uint32_t slot = 16; slot < 26; ++slot) state.invalidate_slot(slot);
    require(state.pending_mask(0x0000FFC0ull) == 0 && state.prepare(0x0000FFC0ull, source) == 0,
            "legal unsampled SetTexture slots do not index or dirty sampled state");
    bool rejected = false;
    try { state.invalidate_slot(26); } catch (const std::out_of_range&) { rejected = true; }
    require(rejected && state.pending_mask(0) == 0, "invalid external slot is rejected without mutating pending work");
    rejected = false;
    try { state.slot(16); } catch (const std::out_of_range&) { rejected = true; }
    require(rejected, "reading a slot outside the sampled snapshot is rejected");

    const auto untouched_version = state.slot(8).version;
    store_word(source, 4, 1, 0xFEDCBA98);
    store_word(source, 9, 4, 0x00000FFF);
    state.invalidate_all(); // Reset or a shader's arbitrary embedded records.
    require(state.pending_mask(0) == State::dirty_mask && state.prepare(0, source) == 384 &&
            state.slot(4).version == 2 && state.slot(9).version == 2 &&
            state.slot(8).version == untouched_version && state.first_mismatch(source) == 96,
            "reset/embedded invalidation refreshes every slot but versions only actual changes");
    state.invalidate_all();
    require(state.prepare(0, source) == 384 && state.slot(4).version == 2 && state.slot(9).version == 2,
            "same-value all invalidation preserves published versions");

    State another_device;
    Source other{};
    require(another_device.prepare(0, other) == 384 && another_device.slot(4).version == 1 &&
            another_device.slot(4).words[1] == 0 && state.slot(4).words[1] == 0xFEDCBA98,
            "new device ownership starts with an independent snapshot");
}

void version_overflow_is_transactional() {
    State state;
    Source source = source_values();
    state.prepare(0, source);
    const auto first = state.slot(0);
    const auto last = state.slot(15).words;
    // Reach the otherwise impractical 2^64-publication boundary without adding a
    // production mutation API. The underlying test state itself is non-const.
    const_cast<State::Slot&>(state.slot(15)).version = (std::numeric_limits<uint64_t>::max)();
    state.invalidate_slot(0);
    state.invalidate_slot(15);
    store_word(source, 0, 0, 0x01020304);
    store_word(source, 15, 5, 0x05060708);
    bool rejected = false;
    try { state.prepare(0, source); } catch (const std::overflow_error&) { rejected = true; }
    require(rejected && state.slot(0).words == first.words && state.slot(0).version == first.version &&
            state.slot(15).words == last && state.pending_mask(0) == 0x80010000ull,
            "overflow in a later dirty slot does not partially publish earlier slots or discard invalidations");
    store_word(source, 15, 5, last[5]);
    require(state.prepare(0, source) == 48 && state.slot(0).version == first.version + 1 &&
            state.slot(15).version == (std::numeric_limits<uint64_t>::max)() && state.pending_mask(0) == 0,
            "unchanged saturated versions can consume dirty state without incrementing or blocking other slots");
}
}

int main() {
    try {
        initial_snapshot_and_clean_draws();
        dirty_slots_and_unconsumed_bits();
        explicit_lifecycle_invalidation();
        version_overflow_is_transactional();
        std::cout << "Event-driven texture fetch state checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
