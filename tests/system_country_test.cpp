#include "system_config.h"
#include "guest_memory.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {
constexpr uint32_t region = 0x10000, buffer = region + 0x20, required = region + 0x40;
constexpr uint32_t small = 0xC0000023, invalid = 0xC00000F1;
using Bytes = std::array<uint8_t, 256>;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void rejects(F operation) {
    try { operation(); } catch (const sfr::RuntimeStop&) { return; }
    throw std::runtime_error("unsupported country configuration did not stop");
}
struct Fixture {
    sfr::GuestMemory memory;
    Fixture() { memory.map(region, 256); reset(); }
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
void expected_store(Bytes& bytes, uint32_t address, uint32_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
        bytes[address - region + i] = static_cast<uint8_t>(value >> (8 * (width - i - 1)));
}

void all_107_countries_publish_exact_one_and_two_bytes() {
    Fixture f;
    unsigned accepted = 0;
    for (uint32_t country = 1; country <= 109; ++country) {
        if (country == 17 || country == 94) continue;
        ++accepted;
        const sfr::SystemConfig config(f.memory, 8, country);
        require(config.country() == std::optional<uint32_t>{country} && config.language() == 8,
                "configured country and language remain independent exact values");
        for (uint16_t capacity : {uint16_t{1}, uint16_t{2}, uint16_t{4}, uint16_t{0xFFFF}}) {
            f.reset();
            auto expected = f.bytes();
            expected_store(expected, buffer, country, 1);
            expected_store(expected, required, 1, 2);
            require(config.query(3, 14, buffer, capacity, required) == 0 && f.bytes() == expected,
                    "country success writes exactly BYTE country then BE16 required length");
        }
    }
    require(accepted == 107, "exercise all accepted country codes");
    const auto before = f.bytes();
    for (uint32_t country : {0u, 17u, 94u, 110u, 256u, UINT32_MAX}) {
        rejects([&] { const sfr::SystemConfig config(f.memory, 8, country); });
        require(f.bytes() == before, "invalid configured country rejects without guest effects");
    }
}

void missing_country_has_no_fallback_and_language_stays_available() {
    Fixture f;
    for (bool explicit_missing : {false, true}) {
        const sfr::SystemConfig config = explicit_missing
            ? sfr::SystemConfig(f.memory, 12, std::nullopt) : sfr::SystemConfig(f.memory, 12);
        require(!config.country().has_value(), "missing country remains visibly unconfigured");
        for (auto request : {std::pair{buffer, uint16_t{1}}, std::pair{buffer, uint16_t{0}},
                             std::pair{0u, uint16_t{0}}, std::pair{0u, uint16_t{1}}}) {
            f.reset();
            const auto before = f.bytes();
            rejects([&] { config.query(3, 14, request.first, request.second, required); });
            rejects([&] { config.query(3, 14, request.first, request.second, 0); });
            require(f.bytes() == before, "unconfigured country stops even size/status-only queries before writes");
        }
        auto expected = f.bytes();
        expected_store(expected, buffer, 12, 4);
        expected_store(expected, required, 4, 2);
        require(config.query(3, 9, buffer, 4, required) == 0 && f.bytes() == expected,
                "existing two-argument language configuration remains usable without country");
    }
    for (uint32_t language = 1; language <= 12; ++language) {
        const sfr::SystemConfig config(f.memory, language, 109);
        f.reset();
        auto expected = f.bytes();
        expected_store(expected, buffer, language, 4);
        expected_store(expected, required, 4, 2);
        require(config.query(3, 9, buffer, 4, required) == 0 && f.bytes() == expected,
                "country configuration does not change four-byte language query behavior");
        f.reset();
        expected = f.bytes();
        expected_store(expected, required, 0, 2);
        require(config.query(3, 9, buffer, 1, required) == small && f.bytes() == expected,
                "language retains its own four-byte capacity threshold");
    }
}

void country_status_paths_do_not_probe_ineffectual_buffers() {
    Fixture f;
    const sfr::SystemConfig config(f.memory, 1, 16);
    for (uint32_t destination : {buffer, 0x34560000u, UINT32_MAX}) {
        f.reset();
        auto expected = f.bytes();
        expected_store(expected, required, 0, 2);
        require(config.query(3, 14, destination, 0, required) == small && f.bytes() == expected,
                "nonnull zero-capacity country buffer is not probed and required becomes zero");
        require(config.query(3, 14, destination, 0, 0) == small && f.bytes() == expected,
                "required pointer is optional on insufficient capacity");
    }
    for (uint16_t capacity : {uint16_t{0}, uint16_t{1}, uint16_t{2}, uint16_t{0xFFFF}}) {
        f.reset();
        auto expected = f.bytes();
        expected_store(expected, required, capacity ? 0 : 1, 2);
        require(config.query(3, 14, 0, capacity, required) == (capacity ? invalid : 0) &&
                    f.bytes() == expected,
                "null buffer distinguishes size query from invalid nonzero capacity");
        require(config.query(3, 14, 0, capacity, 0) == (capacity ? invalid : 0) &&
                    f.bytes() == expected, "null required makes country status/size query effectless");
    }
    for (bool imported : {false, true}) {
        Fixture guarded;
        const sfr::SystemConfig guarded_config(guarded.memory, 1, 18);
        unsigned samples = 0;
        if (imported) guarded.memory.add_import_variable(buffer, "CountryIneffectualBuffer");
        else guarded.memory.add_read_only_word(buffer, [&] { ++samples; return 0u; });
        auto expected = guarded.bytes();
        expected_store(expected, required, 0, 2);
        require(guarded_config.query(3, 14, buffer, 0, required) == small &&
                    guarded.bytes() == expected && samples == 0,
                "ineffectual import/provider buffer must not be read or checked for writing");
    }
}

void exact_mapping_ends_and_nullable_required_are_supported() {
    Fixture f;
    const sfr::SystemConfig config(f.memory, 8, 109);
    f.memory.map(0x20000, 1);
    f.memory.map(0x30000, 2);
    f.memory.map(0xFFFF0000, 0x10000);
    require(config.query(3, 14, 0x20000, 0xFFFF, 0x30000) == 0 &&
                f.memory.load<uint8_t>(0x20000) == 109 && f.memory.load<uint16_t>(0x30000) == 1,
            "only one buffer byte and two required bytes need mapping regardless of capacity");
    require(config.query(3, 14, UINT32_MAX, 1, 0) == 0 &&
                f.memory.load<uint8_t>(UINT32_MAX) == 109,
            "country byte can occupy final guest address with null required");
    require(config.query(3, 14, 0x20000, 1, 0xFFFFFFFE) == 0 &&
                f.memory.load<uint16_t>(0xFFFFFFFE) == 1,
            "BE16 required output may end at guest address-space boundary");
    auto expected = f.bytes();
    expected_store(expected, buffer + 1, 109, 1);
    require(config.query(3, 14, buffer + 1, 1, 0) == 0 && f.bytes() == expected,
            "BYTE output needs no alignment and required remains optional on success");
}

void all_effects_preflight_before_either_output_changes() {
    for (bool first_output : {false, true}) {
        for (bool imported : {false, true}) {
            Fixture f;
            const sfr::SystemConfig config(f.memory, 8, 93);
            unsigned samples = 0;
            const uint32_t guard = first_output ? buffer : required + 4;
            if (imported) f.memory.add_import_variable(guard, "CountryOutput");
            else f.memory.add_read_only_word(guard, [&] { ++samples; return 0u; });
            const auto before = f.bytes();
            rejects([&] { config.query(3, 14, buffer, 1, first_output ? required : required + 3); });
            require(f.bytes() == before && samples == 0,
                    "country or last required byte guard prevents both stores and provider samples");
        }
    }
    Fixture f;
    const sfr::SystemConfig config(f.memory, 8, 95);
    f.memory.map(0x20000, 1);
    f.memory.store<uint8_t>(0x20000, 0xDA);
    f.memory.map(0xFFFF0000, 0x10000);
    f.memory.store<uint8_t>(UINT32_MAX, 0xDA);
    for (auto pair : {std::pair{0x34560000u, required}, std::pair{buffer, 0x20000u},
                      std::pair{buffer, UINT32_MAX}}) {
        const auto before = f.bytes();
        rejects([&] { config.query(3, 14, pair.first, 1, pair.second); });
        require(f.bytes() == before && f.memory.load<uint8_t>(0x20000) == 0xDA &&
                    f.memory.load<uint8_t>(UINT32_MAX) == 0xDA,
                "unmapped byte or partially mapped/overflowing required prevents all writes");
    }
    f.memory.add_read_only_word(required, [] { return 0u; });
    const auto before = f.bytes();
    for (auto request : {std::pair{buffer, uint16_t{0}}, std::pair{0u, uint16_t{1}},
                         std::pair{0u, uint16_t{0}}}) {
        rejects([&] { config.query(3, 14, request.first, request.second, required); });
        require(f.bytes() == before, "country error/size paths also preflight their required write");
    }
}

void overlapping_outputs_preserve_country_then_required_order() {
    Fixture f;
    const sfr::SystemConfig config(f.memory, 8, 109);
    for (int delta : {-2, -1, 0, 1, 2}) {
        f.reset();
        const uint32_t length_output = static_cast<uint32_t>(int(buffer) + delta);
        auto expected = f.bytes();
        expected_store(expected, buffer, 109, 1);
        expected_store(expected, length_output, 1, 2);
        require(config.query(3, 14, buffer, 1, length_output) == 0 && f.bytes() == expected,
                "BE16 required store wins any overlap after the BYTE country store");
    }
    f.reset();
    auto expected = f.bytes();
    expected_store(expected, buffer, 0, 2);
    require(config.query(3, 14, buffer, 0, buffer) == small && f.bytes() == expected,
            "undersized overlapping query writes only its two-byte required output");
}

void live_reservation_and_unknown_queries_preserve_all_state() {
    Fixture f;
    const sfr::SystemConfig config(f.memory, 8, 109);
    const auto reserved = f.memory.load_reserved_word(region);
    const auto before = f.bytes();
    rejects([&] { config.query(3, 14, buffer, 1, required); });
    rejects([&] { config.query(3, 14, buffer, 1, 0); });
    rejects([&] { config.query(3, 14, 0, 0, required); });
    rejects([&] { config.query(3, 14, buffer, 0, required); });
    require(config.query(3, 14, 0, 0, 0) == 0 &&
                config.query(3, 14, 0, 1, 0) == invalid &&
                config.query(3, 14, 0x34560000, 0, 0) == small,
            "effectless country status paths remain usable during a live reservation");
    require(f.bytes() == before && f.memory.has_reservation() &&
                f.memory.store_conditional_word(region, reserved),
            "country queries preserve reservation identity and all output bytes on failure");
    for (auto query : {std::pair<uint16_t, uint16_t>{0, 14}, {2, 14}, {4, 14}, {0xFFFF, 14},
                       {3, 0}, {3, 13}, {3, 15}, {3, 0xFFFF}}) {
        rejects([&] { config.query(query.first, query.second, buffer, 1, required); });
        rejects([&] { config.query(query.first, query.second, 0, 0, 0); });
        require(f.bytes() == before, "unaudited queries do not publish country or required-length fallbacks");
    }
}
}

int main() {
    unsigned failures = 0;
    for (auto test : {all_107_countries_publish_exact_one_and_two_bytes,
                      missing_country_has_no_fallback_and_language_stays_available,
                      country_status_paths_do_not_probe_ineffectual_buffers,
                      exact_mapping_ends_and_nullable_required_are_supported,
                      all_effects_preflight_before_either_output_changes,
                      overlapping_outputs_preserve_country_then_required_order,
                      live_reservation_and_unknown_queries_preserve_all_state}) {
        try { test(); }
        catch (const std::exception& error) { std::cerr << error.what() << '\n'; ++failures; }
    }
    return failures ? 1 : 0;
}
