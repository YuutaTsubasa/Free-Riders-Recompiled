#include "system_config.h"
#include "guest_memory.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
constexpr uint32_t region = 0x10000, buffer = region + 0x20, required = region + 0x40;
constexpr uint32_t success = 0, buffer_small = 0xC0000023u, invalid_parameter = 0xC00000F1u;
using Bytes = std::array<uint8_t, 256>;

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
template<class F> void rejects(F operation) {
    try { operation(); } catch (const sfr::RuntimeStop&) { return; }
    throw std::runtime_error("unsupported system configuration operation did not stop");
}
struct Fixture {
    sfr::GuestMemory memory;
    Fixture() {
        memory.map(region, 256);
        reset();
    }
    void reset() {
        for (uint32_t i = 0; i < 256; ++i)
            memory.store<uint8_t>(region + i, static_cast<uint8_t>(0x80 + i));
    }
    Bytes bytes() const {
        Bytes result{};
        std::copy_n(memory.base() + region, result.size(), result.begin());
        return result;
    }
};
void set_expected(Bytes& bytes, uint32_t address, uint32_t value, uint32_t width) {
    for (uint32_t i = 0; i < width; ++i)
        bytes[address - region + i] = static_cast<uint8_t>(value >> (8 * (width - i - 1)));
}

void all_languages_write_exact_four_and_two_bytes() {
    Fixture f;
    for (uint32_t language = 1; language <= 12; ++language) {
        const sfr::SystemConfig config(f.memory, language);
        require(config.language() == language, "configured language is preserved exactly");
        for (uint16_t capacity : {uint16_t{4}, uint16_t{5}, uint16_t{0xFFFF}}) {
            f.reset();
            auto expected = f.bytes();
            set_expected(expected, buffer, language, 4);
            set_expected(expected, required, 4, 2);
            require(config.query(3, 9, buffer, capacity, required) == success,
                    "audited language query succeeds at capacity four or greater");
            require(f.bytes() == expected, "success changes only BE32 language and BE16 required length");
        }
    }
    const auto before = f.bytes();
    for (uint32_t language : {0u, 13u, 0x10001u, UINT32_MAX}) {
        rejects([&] { sfr::SystemConfig invalid(f.memory, language); });
        require(f.bytes() == before, "invalid language construction has no guest effects");
    }
}

void insufficient_capacity_does_not_probe_or_write_buffer() {
    Fixture f;
    const sfr::SystemConfig config(f.memory, 12);
    constexpr uint32_t unmapped = 0x34560000;
    for (uint16_t capacity = 0; capacity < 4; ++capacity) {
        for (uint32_t destination : {buffer, unmapped, UINT32_MAX}) {
            f.reset();
            auto expected = f.bytes();
            set_expected(expected, required, 0, 2);
            require(config.query(3, 9, destination, capacity, required) == buffer_small,
                    "non-null undersized buffer returns exact status even when its address is inaccessible");
            require(f.bytes() == expected, "undersized request only writes zero to the optional BE16 length");
            const auto before = f.bytes();
            require(config.query(3, 9, destination, capacity, 0) == buffer_small,
                    "required pointer may be null on undersized requests");
            require(f.bytes() == before, "null required pointer makes undersized request effectless");
        }
    }
    unsigned samples = 0;
    f.memory.add_read_only_word(buffer, [&] { ++samples; return 0u; });
    auto expected = f.bytes();
    set_expected(expected, required, 0, 2);
    require(config.query(3, 9, buffer, 3, required) == buffer_small,
            "ineffectual readonly buffer is not preflighted");
    require(samples == 0 && f.bytes() == expected, "ineffectual buffer provider is never sampled");
}

void null_buffer_size_queries_and_invalid_capacities_are_distinct() {
    Fixture f;
    const sfr::SystemConfig config(f.memory, 4);
    for (uint16_t capacity : {uint16_t{0}, uint16_t{1}, uint16_t{3}, uint16_t{4}, uint16_t{0xFFFF}}) {
        f.reset();
        auto expected = f.bytes();
        set_expected(expected, required, capacity == 0 ? 4 : 0, 2);
        const uint32_t status = capacity == 0 ? success : invalid_parameter;
        require(config.query(3, 9, 0, capacity, required) == status,
                "null buffer permits only the zero-capacity size query");
        require(f.bytes() == expected, "null buffer path writes exactly the reported BE16 size");
        const auto before = f.bytes();
        require(config.query(3, 9, 0, capacity, 0) == status,
                "required output is optional on every null-buffer path");
        require(f.bytes() == before, "both null outputs cause no memory effects");
    }
    f.reset();
    auto expected = f.bytes();
    set_expected(expected, buffer, 4, 4);
    require(config.query(3, 9, buffer, 4, 0) == success, "successful buffer query permits null required output");
    require(f.bytes() == expected, "null required output does not alter surrounding bytes");
}

void exact_mapping_extents_and_guest_end_are_respected() {
    sfr::GuestMemory memory;
    memory.map(0x20000, 4);
    memory.map(0x30000, 2);
    memory.map(0xFFFFF000, 0x1000);
    const sfr::SystemConfig config(memory, 11);
    require(config.query(3, 9, 0x20000, 0xFFFF, 0x30000) == success,
            "capacity is not the written extent: four mapped buffer bytes and two required bytes suffice");
    require(memory.load<uint32_t>(0x20000) == 11 && memory.load<uint16_t>(0x30000) == 4,
            "exactly bounded outputs contain expected values");
    require(config.query(3, 9, 0xFFFFFFFC, 4, 0) == success,
            "BE32 output can end exactly at the guest address-space boundary");
    require(memory.load<uint32_t>(0xFFFFFFFC) == 11, "last four guest bytes contain the language");
    require(config.query(3, 9, 0, 0, 0xFFFFFFFE) == success,
            "BE16 required length can end exactly at the guest address-space boundary");
    memory.store<uint32_t>(0x20000, 0xDEADBEEF);
    memory.store<uint16_t>(0x30000, 0xA1B2);
    const uint32_t end_before = memory.load<uint32_t>(0xFFFFFFFC);
    rejects([&] { config.query(3, 9, 0xFFFFFFFD, 4, 0x30000); });
    require(memory.load<uint32_t>(0xFFFFFFFC) == end_before && memory.load<uint16_t>(0x30000) == 0xA1B2,
            "buffer crossing guest end leaves both outputs unchanged");
    rejects([&] { config.query(3, 9, 0x20000, 4, UINT32_MAX); });
    require(memory.load<uint32_t>(0x20000) == 0xDEADBEEF && memory.load<uint32_t>(0xFFFFFFFC) == end_before,
            "late required output crossing guest end is checked before any buffer write");
}

void partial_mapping_and_late_required_guards_are_transactional() {
    for (bool guard_required : {false, true}) {
        Fixture f;
        f.memory.map(0x20000, guard_required ? 1 : 3);
        f.memory.store<uint8_t>(0x20000, 0xDA);
        const sfr::SystemConfig config(f.memory, 5);
        const auto before = f.bytes();
        rejects([&] { config.query(3, 9, guard_required ? buffer : 0x20000, 4,
                                   guard_required ? 0x20000 : required); });
        require(f.bytes() == before && f.memory.load<uint8_t>(0x20000) == 0xDA,
                "partially mapped first or last output fails before either output changes");
    }
    for (bool imported : {false, true}) {
        for (bool guard_required : {false, true}) {
            Fixture f;
            const sfr::SystemConfig config(f.memory, 6);
            unsigned samples = 0;
            const uint32_t guarded = guard_required ? required + 4 : buffer;
            if (imported) f.memory.add_import_variable(guarded, "SystemConfigOutputGuard");
            else f.memory.add_read_only_word(guarded, [&] { ++samples; return 0u; });
            const auto before = f.bytes();
            // Required begins one byte before the guarded word, testing its full two-byte extent.
            const uint32_t length_output = guard_required ? required + 3 : required;
            rejects([&] { config.query(3, 9, buffer, 4, length_output); });
            require(f.bytes() == before && samples == 0,
                    "provider/import on either output prevents all writes and provider reads");
        }
    }
    Fixture f;
    const sfr::SystemConfig config(f.memory, 1);
    unsigned samples = 0;
    f.memory.add_read_only_word(required, [&] { ++samples; return 0u; });
    const auto before = f.bytes();
    for (auto request : {std::pair{buffer, uint16_t{3}}, std::pair{0u, uint16_t{1}},
                         std::pair{0u, uint16_t{0}}}) {
        rejects([&] { config.query(3, 9, request.first, request.second, required); });
        require(f.bytes() == before && samples == 0,
                "status-only and size-only paths still preflight their required-length write");
    }
}

void overlaps_preserve_original_buffer_then_required_store_order() {
    Fixture f;
    const sfr::SystemConfig config(f.memory, 12);
    // Includes either edge, interior overlaps, and non-overlapping neighbors.
    for (int delta : {-2, -1, 0, 1, 2, 3, 4}) {
        f.reset();
        const uint32_t length_output = static_cast<uint32_t>(int(buffer) + delta);
        auto expected = f.bytes();
        set_expected(expected, buffer, 12, 4);
        set_expected(expected, length_output, 4, 2);
        require(config.query(3, 9, buffer, 4, length_output) == success,
                "original wrapper permits overlapping outputs");
        require(f.bytes() == expected, "required BE16 write wins overlaps after BE32 buffer store");
    }
    f.reset();
    auto expected = f.bytes();
    set_expected(expected, buffer + 1, 0, 2);
    require(config.query(3, 9, buffer, 1, buffer + 1) == buffer_small,
            "undersized alias still writes only required length");
    require(f.bytes() == expected, "undersized aliased buffer is affected solely through required output");
}

void live_reservation_blocks_only_effectful_queries_and_is_preserved() {
    Fixture f;
    const sfr::SystemConfig config(f.memory, 7);
    const uint32_t reserved_value = f.memory.load_reserved_word(region);
    const auto before = f.bytes();
    rejects([&] { config.query(3, 9, buffer, 4, required); });
    rejects([&] { config.query(3, 9, buffer, 4, 0); });
    rejects([&] { config.query(3, 9, 0, 0, required); });
    rejects([&] { config.query(3, 9, 0x34560000, 1, required); });
    require(f.bytes() == before && f.memory.has_reservation(),
            "all effectful query failures preserve outputs and the live reservation");
    require(config.query(3, 9, 0, 0, 0) == success &&
            config.query(3, 9, 0, 1, 0) == invalid_parameter &&
            config.query(3, 9, 0x34560000, 1, 0) == buffer_small,
            "effectless status/size queries do not reject merely because an atomic reservation exists");
    require(f.bytes() == before && f.memory.has_reservation() &&
            f.memory.store_conditional_word(region, reserved_value),
            "query preserves reservation identity for the following conditional store");
}

void unknown_categories_and_settings_never_publish_fallback_data() {
    Fixture f;
    const sfr::SystemConfig config(f.memory, 8);
    for (auto query : {std::pair<uint16_t, uint16_t>{0, 9}, {1, 9}, {2, 9}, {4, 9},
                       {0xFFFF, 9}, {3, 0}, {3, 8}, {3, 10}, {3, 0xFFFF}}) {
        for (uint16_t capacity : {uint16_t{0}, uint16_t{3}, uint16_t{4}}) {
            f.reset();
            const auto before = f.bytes();
            rejects([&] { config.query(query.first, query.second, buffer, capacity, required); });
            require(f.bytes() == before, "unaudited query fails without fallback language or required-length writes");
            rejects([&] { config.query(query.first, query.second, 0, 0, 0); });
        }
    }
}
}

int main() {
    unsigned failures = 0;
    for (auto test : {all_languages_write_exact_four_and_two_bytes,
                      insufficient_capacity_does_not_probe_or_write_buffer,
                      null_buffer_size_queries_and_invalid_capacities_are_distinct,
                      exact_mapping_extents_and_guest_end_are_respected,
                      partial_mapping_and_late_required_guards_are_transactional,
                      overlaps_preserve_original_buffer_then_required_store_order,
                      live_reservation_blocks_only_effectful_queries_and_is_preserved,
                      unknown_categories_and_settings_never_publish_fallback_data}) {
        try { test(); }
        catch (const std::exception& error) { std::cerr << error.what() << '\n'; ++failures; }
    }
    return failures ? 1 : 0;
}
