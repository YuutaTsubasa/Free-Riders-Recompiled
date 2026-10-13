#include "guest_constant_state.h"
#include <iostream>

static void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}

static void short_constant_checks() {
    using State = sfr::GuestShortConstantState;
    State state, another_device;
    static_assert(State::source_bytes == 96);
    // Include the next PS word in backing storage, but keep it outside the
    // source span passed to the VS consumer and verifier.
    std::array<uint8_t, State::source_bytes + 4> backing;
    backing.fill(0xa5);
    const std::span<const uint8_t, State::source_bytes> guest(backing.data(), State::source_bytes);
    const auto store_word = [&](uint32_t offset, uint32_t value) {
        for (uint32_t i = 0; i < 4; ++i) backing[offset + i] = uint8_t(value >> (24 - i * 8));
    };
    store_word(0, 0xabcd8001);
    store_word(16, 0x98768001);
    store_word(28, 0x55667788); // Old incorrect i0 address: device+10140.
    store_word(96, 0x99aabbcc); // First PS loop at device+10208.
    for (uint32_t i = 0; i < 16; ++i)
        store_word(32 + i * 4, ((0x80u + i) << 24) | ((0x7fu - i) << 16) | ((0xffu - i) << 8) | i);
    store_word(32, 0x7f00ff80);
    store_word(92, 0x80ff007f);
    require(state.version() != 0 && state.prepare(guest) == 72,
            "first short-constant snapshot reads exactly the eighteen relevant words");
    require(state.booleans() == 0x80018001,
            "both shader stages retain bool bits zero and fifteen, ignoring upper word bits");
    constexpr std::array<int32_t, 4> first_lanes{-128, -1, 0, 127}, last_lanes{127, 0, -1, -128};
    for (uint32_t lane = 0; lane < 4; ++lane)
        require(state.loop_values()[lane] == first_lanes[lane] && state.loop_values()[60 + lane] == last_lanes[lane],
                "first and last loops decode all signed-byte endpoints in low-byte-first order");
    for (uint32_t i = 1; i < 15; ++i)
        require(state.loop_values()[i * 4] == int32_t(i) &&
                state.loop_values()[i * 4 + 1] == -1 - int32_t(i) &&
                state.loop_values()[i * 4 + 2] == 127 - int32_t(i) &&
                state.loop_values()[i * 4 + 3] == -128 + int32_t(i),
                "all sixteen packed loop registers preserve their own signed components");
    require(state.first_mismatch(guest) == State::source_bytes, "short-constant verifier accepts the full snapshot");
    const auto initial_loops = state.loop_values();
    store_word(28, 0xdeadbeef);
    store_word(96, 0x10203040);
    require(state.first_mismatch(guest) == State::source_bytes,
            "verification ignores the old i0 address and the PS first loop outside the source span");
    state.invalidate();
    require(state.prepare(guest) == 72 && state.loop_values() == initial_loops && state.booleans() == 0x80018001,
            "refreshing VS loops never depends on the old offset28 sentinel or adjacent PS loop at offset96");
    store_word(32, 0x01020304);
    require(state.first_mismatch(guest) == 32, "the first packed VS loop at device+10144 is verified");
    store_word(32, 0x7f00ff80);
    const auto initial_version = state.version();
    store_word(4, 0x12345678); // Unused bool words are outside the native shader contract.
    store_word(20, 0x87654321);
    require(state.first_mismatch(guest) == State::source_bytes,
            "verification ignores the gaps between the relevant bool and loop words");
    store_word(0, 0xffff0000);
    store_word(16, 0x1234ffff);
    store_word(32, 0x01020304);
    require(state.prepare(guest) == 0 && state.version() == initial_version &&
            state.booleans() == 0x80018001 && state.loop_values() == initial_loops,
            "without invalidation a clean prepare reads nothing and does not silently adopt changed source data");
    require(state.first_mismatch(guest) == 0, "correctness mode reports an untracked VS bool writer");
    state.invalidate_mask(0);
    state.invalidate_mask(~State::dirty_mask);
    require(state.version() == initial_version && state.prepare(guest) == 0,
            "unrelated device dirty bits cannot invalidate short constants");
    state.invalidate_mask(State::dirty_mask | 1);
    require(state.version() != initial_version && state.prepare(guest) == 72 &&
            state.booleans() == 0xffff0000 && state.loop_values()[0] == 4 && state.loop_values()[3] == 1,
            "the shared dirty group refreshes VS bool, PS bool and loops together");
    require(state.first_mismatch(guest) == State::source_bytes && state.prepare(guest) == 0,
            "consumed short-constant state is clean for later draws");
    store_word(16, 0x56780000);
    require(state.first_mismatch(guest) == 16, "PS bool mismatch uses a source byte offset");
    state.invalidate();
    require(state.prepare(guest) == 72 && state.booleans() == 0,
            "reset or shader-embedded invalidation refreshes all short constants");
    store_word(92, 0x7f80ff01);
    require(state.first_mismatch(guest) == 92, "the final packed VS loop at device+10204 is verified");
    state.invalidate(); state.prepare(guest);
    const auto unchanged_version = state.version();
    state.invalidate_mask(State::dirty_mask);
    require(state.version() != unchanged_version && state.prepare(guest) == 72,
            "even a same-value update creates a new version without content-comparison reuse");

    sfr::VersionedLoopConstantUpload upload;
    constexpr size_t loop_bytes = sizeof(State::LoopValues);
    static_assert(loop_bytes == 256);
    std::array<uint8_t, 4 * loop_bytes> ring;
    ring.fill(0xa5);
    require(!upload.matches(&state, state.version()), "a loop allocation starts without a bound owner");
    const auto first = upload.upload(&state, state.version(), state.loop_values(), 0, ring.data());
    require(!first.reused && first.offset == 0 && upload.matches(&state, state.version()),
            "loop uploads preserve offset zero and their versioned owner");
    const auto hit = upload.upload(&state, state.version(), state.loop_values(), loop_bytes, nullptr);
    require(hit.reused && hit.offset == 0 && ring[loop_bytes] == 0xa5,
            "a loop upload hit does not need or touch a destination");
    const auto queued = state.loop_values();
    store_word(32, 0x80808080);
    state.invalidate_mask(State::dirty_mask); state.prepare(guest);
    require(!upload.upload(&state, state.version(), state.loop_values(), loop_bytes, ring.data() + loop_bytes).reused,
            "a loop update receives a new immutable upload slot");
    require(std::memcmp(ring.data(), queued.data(), loop_bytes) == 0 &&
            std::memcmp(ring.data() + loop_bytes, state.loop_values().data(), loop_bytes) == 0,
            "queued loop bytes stay unchanged while the next draw receives the updated values");
    require(!upload.upload(&another_device, state.version(), state.loop_values(), 2 * loop_bytes,
                           ring.data() + 2 * loop_bytes).reused,
            "equal loop version numbers from different owners do not alias");
    upload.reset();
    require(!upload.matches(&another_device, state.version()) &&
            !upload.upload(&another_device, state.version(), state.loop_values(), 3 * loop_bytes,
                           ring.data() + 3 * loop_bytes).reused,
            "a new ring lifetime uploads the unchanged loop version again");
}

static void sparse_float_constant_checks() {
    using State = sfr::GuestConstantState;
    State state;
    std::array<uint8_t, State::words * 4> guest{};
    State::Values source_values{}, expected{};
    const auto fill_source = [&](uint32_t seed) {
        for (uint32_t word = 0; word < State::words; ++word) {
            const uint32_t value = 0x9e3779b9u * (word + 1) ^ seed;
            source_values[word] = value;
            for (uint32_t byte = 0; byte < 4; ++byte)
                guest[word * 4 + byte] = uint8_t(value >> (24 - byte * 8));
        }
    };
    fill_source(0x7fc00001);
    expected = source_values;
    const uint64_t initial_version = state.version();
    state.invalidate_mask(1);
    state.invalidate(4, 4);
    require(state.version() == initial_version && state.prepare(guest) == 4096 && state.values() == expected,
            "first publication remains fully dirty and repeated initial events keep the same pending version");

    uint32_t seed = 0;
    const auto exercise = [&](uint64_t mask, uint32_t first, uint32_t count) {
        // Change all source words, including unflagged ones. A scalar per-word
        // oracle identifies the exact union without reproducing the run merger.
        fill_source(++seed * 0x1020304u);
        uint32_t expected_bytes = 0;
        for (uint32_t word = 0; word < State::words; ++word) {
            const bool dirty = ((mask >> (63 - word / 16)) & 1) ||
                               (first <= word && word - first < count);
            if (dirty) {
                expected[word] = source_values[word];
                expected_bytes += 4;
            }
        }
        const uint64_t before = state.version();
        state.invalidate_mask(mask);
        state.invalidate(first, count);
        require(state.version() == before + (expected_bytes != 0),
                "mask and explicit updates acquire one new version only when clean state becomes pending");
        state.invalidate(first, count);
        state.invalidate_mask(mask);
        require(state.version() == before + (expected_bytes != 0),
                "repeated setters and original dirty flags coalesce until publication");
        require(state.prepare(guest) == expected_bytes && state.values() == expected,
                "SIMD runs match the complete 1024-word oracle while leaving unflagged words unchanged");
        require(state.prepare(guest) == 0 && state.version() == before + (expected_bytes != 0),
                "publication consumes all pending events without changing their version again");
        uint32_t first_mismatch = State::words;
        for (uint32_t word = 0; word < State::words; ++word)
            if (expected[word] != source_values[word]) { first_mismatch = word; break; }
        require(state.first_mismatch(guest) == first_mismatch,
                "verification reports the first unflagged write even after sparse publication");
    };
    struct Case { uint64_t mask; uint32_t first, count; };
    constexpr Case cases[] = {
        {0x8000000000000001ull, 0, 0}, // Sparse first/last groups, 128 bytes.
        {0, 17, 1},                    // Scalar word tail, no group rounding.
        {0, 3, 19},                    // Unaligned SIMD run with scalar tail.
        {0x8000000000000000ull, 5, 5}, // Explicit interval inside a dirty group.
        {0x8000000000000000ull, 16, 3},// Adjacent interval after a group.
        {0x4000000000000000ull, 13, 3},// Adjacent interval before a group.
        {0xa000000000000000ull, 12, 25},// Explicit interval bridges two groups.
        {0xa000000000000000ull, 17, 2}, // Preserve gaps on both sides of interval.
        {0x4000000000000000ull, 1000, 3},// Separate explicit interval after runs.
        {1, 7, 5},                    // Separate explicit interval before runs.
        {1, 1004, 4},                 // Adjacency at the final group.
        {0xaaaaaaaaaaaaaaaaull, 100, 20},
        {0x5555555555555555ull, 27, 990},
        {~uint64_t{0}, 511, 7},        // Complete mask absorbs the interval.
        {0, 1023, 1},                  // Final scalar word.
        {0, 0, 1024},                  // Explicit full reset.
        {0, 1024, 0},                  // Legal empty range is a true no-op.
    };
    for (const auto& entry : cases) exercise(entry.mask, entry.first, entry.count);
    for (uint32_t group = 0; group < 64; ++group)
        exercise(uint64_t{1} << (63 - group), 0, 0);

    const uint64_t before_bad_range = state.version();
    constexpr std::array<std::array<uint32_t, 2>, 5> bad_ranges{{
        {1025, 0}, {1023, 2}, {0, 0xffffffffu}, {0xffffffffu, 2}, {1, 0xffffffffu}}};
    for (const auto& range : bad_ranges) {
        bool rejected = false;
        try { state.invalidate(range[0], range[1]); } catch (const std::out_of_range&) { rejected = true; }
        require(rejected && state.version() == before_bad_range && state.prepare(guest) == 0 && state.values() == expected,
                "invalid and overflowing ranges do not change clean versions, pending ranges or published words");
    }
    fill_source(0x87654321);
    state.invalidate(511, 3);
    const uint64_t pending_version = state.version();
    bool rejected = false;
    try { state.invalidate(1, 0xffffffffu); } catch (const std::out_of_range&) { rejected = true; }
    for (uint32_t word = 511; word < 514; ++word) expected[word] = source_values[word];
    require(rejected && pending_version == before_bad_range + 1 && state.version() == pending_version &&
            state.prepare(guest) == 12 && state.values() == expected,
            "failed range arithmetic preserves an existing exact pending interval and its version");

    state.invalidate(20, 3);
    const uint64_t coalesced_version = state.version();
    state.invalidate(21, 5);
    state.invalidate(26, 2);
    for (uint32_t word = 20; word < 28; ++word) expected[word] = source_values[word];
    require(state.version() == coalesced_version && state.prepare(guest) == 32 && state.values() == expected,
            "overlapping and adjacent explicit setters retain one precise interval and one pending version");
}

static void direct_float_constant_checks() {
    using State = sfr::GuestConstantState;
    constexpr uint32_t bytes = State::words * 4;
    State state, another_device;
    std::array<uint8_t, bytes> guest{};
    State::Values source_values{};
    const auto store_word = [&](uint32_t word, uint32_t value) {
        source_values[word] = value;
        for (uint32_t byte = 0; byte < 4; ++byte)
            guest[word * 4 + byte] = uint8_t(value >> (24 - byte * 8));
    };
    for (uint32_t word = 0; word < State::words; ++word)
        store_word(word, 0x9e3779b9u * (word + 1));
    store_word(0, 0x80000000); // Exact negative zero and NaN payloads remain bits.
    store_word(1, 0x7fc01234);
    store_word(1023, 0xff800001);

    const uint64_t initial_version = state.version();
    require(state.publish_version() == initial_version && state.publish_version() == initial_version &&
            state.values() == State::Values{}, "metadata publication does not read or populate the CPU shadow");
    // The shadow is still initially fully dirty. That must not prevent version
    // changes after metadata-only publication has consumed a previous event.
    for (uint32_t event = 0; event < 4; ++event) {
        const uint64_t previous = state.version();
        state.invalidate_mask(uint64_t{1} << (63 - event));
        state.invalidate(event * 16, 2);
        state.invalidate_mask(uint64_t{1} << (63 - event));
        require(state.version() == previous + 1 && state.publish_version() == previous + 1 &&
                state.values() == State::Values{},
                "metadata-only event cycles coalesce within each publication but advance despite an unmaterialized shadow");
    }
    const uint64_t published = state.version();
    require(state.prepare(guest) == bytes && state.version() == published && state.values() == source_values &&
            state.prepare(guest) == 0, "switching to the first shadow snapshot does not create another logical version");
    const auto baseline = state.values();

    store_word(0, 0x3f800000);
    store_word(511, 0xdeadbeef); // No setter or dirty bit for this middle word.
    state.invalidate_mask(uint64_t{1} << 63);
    require(state.publish_version() == published + 1, "first deferred sparse update advances the metadata version");
    store_word(1023, 0x7fc00001);
    state.invalidate_mask(1);
    require(state.publish_version() == published + 2 && state.values() == baseline,
            "later metadata publication retains earlier deferred shadow ranges without materializing them");
    sfr::VersionedConstantUpload verifier_upload;
    std::array<uint8_t, bytes> direct_ring{};
    require(!verifier_upload.upload_source(&state, state.version(), guest, 0, direct_ring.data()).reused &&
            std::memcmp(direct_ring.data(), source_values.data(), bytes) == 0 && state.values() == baseline,
            "full direct conversion copies all source words synchronously but never populates the sparse oracle");
    require(state.prepare(guest) == 128 && state.version() == published + 2 &&
            state.values()[0] == source_values[0] && state.values()[1023] == source_values[1023] &&
            state.values()[511] == baseline[511] && state.first_mismatch(guest) == 511,
            "deferred sparse preparation preserves both endpoint events and still detects an unflagged writer after direct upload");
    store_word(511, baseline[511]);
    require(state.first_mismatch(guest) == State::words, "repairing the unflagged test write restores oracle agreement");
    state.invalidate(18, 1);
    const uint64_t next_version = state.version();
    store_word(18, 0x12345678);
    require(next_version == published + 3 && state.prepare(guest) == 4 && state.first_mismatch(guest) == State::words,
            "the original exact sparse path remains usable after metadata-only cycles");

    sfr::VersionedConstantUpload upload;
    std::array<uint8_t, 4 * bytes> ring;
    ring.fill(0xa5);
    const auto queued = source_values;
    const auto first = upload.upload_source(&state, state.version(), guest, 0, ring.data());
    require(!first.reused && first.offset == 0 && std::memcmp(ring.data(), queued.data(), bytes) == 0,
            "a direct cache miss publishes an exact immutable full-stage allocation including offset zero");
    store_word(1, 0x7fc05678);
    const auto hit = upload.upload_source(&state, state.version(), {}, bytes, nullptr);
    require(hit.reused && hit.offset == 0 && std::memcmp(ring.data(), queued.data(), bytes) == 0 && ring[bytes] == 0xa5,
            "a clean allocation hit requires neither guest source nor destination and preserves queued bytes");

    state.invalidate(1, 1);
    const uint64_t changed_version = state.publish_version();
    const auto expect_invalid_miss = [&](std::span<const uint8_t> source, uint8_t* destination) {
        bool rejected = false;
        try { upload.upload_source(&state, changed_version, source, bytes, destination); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected && upload.matches(&state, next_version) && !upload.matches(&state, changed_version) &&
                std::all_of(ring.begin() + bytes, ring.end(), [](uint8_t value) { return value == 0xa5; }),
                "an invalid direct miss preserves the previous owner/version and does not partially write the new destination");
    };
    expect_invalid_miss({}, ring.data() + bytes);
    expect_invalid_miss(std::span<const uint8_t>(guest).first(bytes - 1), ring.data() + bytes);
    expect_invalid_miss(guest, nullptr);
    const auto changed = upload.upload_source(&state, changed_version, guest, bytes, ring.data() + bytes);
    require(!changed.reused && changed.offset == bytes && upload.matches(&state, changed_version) &&
            std::memcmp(ring.data(), queued.data(), bytes) == 0 &&
            std::memcmp(ring.data() + bytes, source_values.data(), bytes) == 0,
            "a successful retry publishes the new direct version without overwriting the earlier draw");

    const auto different_owner = upload.upload_source(&another_device, changed_version, guest, 2 * bytes, ring.data() + 2 * bytes);
    require(!different_owner.reused && different_owner.offset == 2 * bytes,
            "matching version numbers from different owners cannot reuse direct uploads");
    for (const bool missing_owner : {false, true}) {
        bool rejected = false;
        try { upload.upload_source(missing_owner ? nullptr : &another_device, missing_owner ? changed_version : 0,
                                   guest, 3 * bytes, ring.data() + 3 * bytes); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected && upload.matches(&another_device, changed_version),
                "invalid direct upload identity cannot replace a valid cached allocation");
    }
    upload.reset();
    bool rejected = false;
    try { upload.upload_source(&another_device, changed_version, {}, 3 * bytes, ring.data() + 3 * bytes); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && !upload.matches(&another_device, changed_version) && ring[3 * bytes] == 0xa5,
            "a reset ring cannot reuse the old offset and requires a complete source even for an unchanged version");
    require(!upload.upload_source(&another_device, changed_version, guest, 3 * bytes, ring.data() + 3 * bytes).reused &&
            std::memcmp(ring.data() + 3 * bytes, source_values.data(), bytes) == 0,
            "the unchanged direct version is converted into the new ring after reset");
    const auto last_snapshot = source_values;
    guest.fill(0);
    require(std::memcmp(ring.data() + bytes, last_snapshot.data(), bytes) == 0 &&
            std::memcmp(ring.data() + 3 * bytes, last_snapshot.data(), bytes) == 0,
            "changing guest storage after synchronous direct upload cannot change recorded allocations");
}

int main() {
    try {
        sfr::GuestConstantState stage, another_device;
        std::array<uint8_t, 4096> guest{};
        guest[4092] = 0x80; // negative zero in the last register
        require(stage.prepare(guest) == 4096, "first draw reads the entire initial state");
        require(stage.values().back() == 0x80000000, "last register and endian conversion");
        require(stage.prepare(guest) == 0, "unchanged draws read no guest bytes");
        const uint64_t before = stage.version();
        guest[16] = 0x3f; guest[17] = 0x80;
        stage.invalidate(4, 4);
        require(stage.version() != before, "partial update changes stage version");
        require(stage.prepare(guest) == 16, "a float4 update reads exactly 16 bytes");
        require(stage.values()[4] == 0x3f800000 && stage.values().back() == 0x80000000,
                "partial update preserves other registers");
        require(stage.first_mismatch(guest) == 1024, "event state agrees with guest shadow");
        guest[300] = 1;
        require(stage.first_mismatch(guest) == 75, "validation finds an unhooked direct store");
        stage.invalidate();
        require(stage.prepare(guest) == 4096, "shader embedded constants/reset invalidates the stage");
        const uint64_t unchanged = stage.version();
        stage.invalidate(1024, 0);
        require(stage.version() == unchanged && stage.prepare(guest) == 0, "empty update is a no-op");
        bool rejected = false;
        try { stage.invalidate(1023, 2); } catch (const std::out_of_range&) { rejected = true; }
        require(rejected && stage.version() == unchanged, "bad range fails before changing state");
        stage.invalidate_mask(0);
        require(stage.version() == unchanged, "clean original mask is a no-op");
        guest[3264] = 0x42; // c204.x, inline store at device + 5184
        stage.invalidate_mask(1ull << 12);
        require(stage.prepare(guest) == 64 && stage.values()[816] == 0x42000000,
                "inline c204 update uses original most-significant-bit-first mask");
        stage.invalidate_mask(1ull << 63);
        require(stage.prepare(guest) == 64, "first mask bit covers c0..c3");
        stage.invalidate_mask(1);
        require(stage.prepare(guest) == 64, "last mask bit covers c252..c255");
        guest[2048] = 0x7f;
        stage.invalidate_mask((1ull << 63) | 1);
        require(stage.prepare(guest) == 128 && stage.values()[512] == 0 && stage.first_mismatch(guest) == 512,
                "disjoint endpoint updates read only 128 bytes and do not adopt an unflagged middle write");
        guest[2048] = 0;

        sfr::VersionedConstantUpload upload;
        std::array<uint8_t, 4 * 4096> ring;
        ring.fill(0xa5);
        const auto first = upload.upload(&stage, stage.version(), stage.values(), 0, ring.data());
        require(!first.reused && first.offset == 0, "first upload includes offset zero");
        const auto hit = upload.upload(&stage, stage.version(), stage.values(), 4096, ring.data() + 4096);
        require(hit.reused && hit.offset == 0 && ring[4096] == 0xa5, "same version does not touch destination");
        const auto old = stage.values();
        guest[0] = 0x7f; guest[1] = 0xc0; guest[3] = 1; // exact NaN payload
        stage.invalidate(0, 4); stage.prepare(guest);
        require(!upload.upload(&stage, stage.version(), stage.values(), 4096, ring.data() + 4096).reused,
                "update after queued draw allocates another slot");
        require(std::memcmp(ring.data(), old.data(), 4096) == 0, "earlier queued draw's slot remains immutable");
        require(!upload.upload(&another_device, stage.version(), stage.values(), 8192, ring.data() + 8192).reused,
                "the same version number from another owner cannot alias");
        upload.reset();
        require(!upload.upload(&another_device, stage.version(), stage.values(), 12288, ring.data() + 12288).reused,
                "ring reuse requires another upload even when version matches");
        sparse_float_constant_checks();
        direct_float_constant_checks();
        short_constant_checks();
        std::cout << "Event-driven constant lifetime checks passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
