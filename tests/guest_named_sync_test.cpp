#include "guest_sync_objects.h"
#include "guest_memory.h"
#include "native_sync_objects.h"
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
constexpr uint32_t region = 0x10000000, extent = 0x1000;
constexpr uint32_t output = region + 0x20, attributes = region + 0x100;
constexpr uint32_t descriptor = region + 0x200, name_bytes = region + 0x300;
constexpr uint32_t exists = 0x40000000, first_handle = 0x72100004;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void rejects(F operation) {
    try { operation(); } catch (const sfr::RuntimeStop&) { return; }
    throw std::runtime_error("unsupported named synchronization request did not stop");
}
struct Fixture {
    sfr::GuestMemory memory;
    sfr::NativeSyncObjects native;
    sfr::GuestSyncObjects guest{memory, native};
    Fixture() {
        memory.map(region, extent);
        std::fill_n(memory.base() + region, extent, uint8_t{0xDA});
        set_name("GuestNamedSync");
    }
    void set_name(std::string_view name, uint32_t address = name_bytes) {
        memory.store<uint32_t>(attributes, 0xFFFFFFFC);
        memory.store<uint32_t>(attributes + 4, descriptor);
        memory.store<uint32_t>(attributes + 8, 0x80);
        memory.store<uint16_t>(descriptor, static_cast<uint16_t>(name.size()));
        memory.store<uint16_t>(descriptor + 2, static_cast<uint16_t>(name.size()));
        memory.store<uint32_t>(descriptor + 4, address);
        for (size_t i = 0; i < name.size(); ++i)
            memory.store<uint8_t>(uint64_t(address) + i, static_cast<uint8_t>(name[i]));
    }
    std::vector<uint8_t> bytes() const {
        return {memory.base() + region, memory.base() + region + extent};
    }
    uint32_t create(bool event, uint32_t out = output, uint32_t attrs = attributes) {
        return event ? guest.create_event(out, attrs, 1, 0)
                     : guest.create_semaphore(out, attrs, 1, 2);
    }
    void failure_without_effects(bool event, uint32_t out = output, uint32_t attrs = attributes) {
        const auto before = bytes();
        rejects([&] { create(event, out, attrs); });
        require(bytes() == before && native.open_count() == 0,
                "failed named preflight must preserve all guest bytes and create no object");
        require(guest.create_semaphore(output, 0, 0, 1) == 0 &&
                    memory.load<uint32_t>(output) == first_handle,
                "failed named preflight must not consume a guest handle ID");
    }
};
void expected_handle(std::vector<uint8_t>& bytes, uint32_t address, uint32_t handle) {
    for (unsigned i = 0; i < 4; ++i)
        bytes[address - region + i] = static_cast<uint8_t>(handle >> (24 - 8 * i));
}

void named_creation_writes_exact_be_handle_and_unnamed_still_works() {
    for (bool event : {false, true}) {
        Fixture f;
        auto expected = f.bytes();
        expected_handle(expected, output, first_handle);
        require(f.create(event) == 0, "first named object creation must succeed");
        require(f.bytes() == expected && f.native.open_count() == 1 && f.native.owns(first_handle),
                "named creation publishes exactly four big-endian handle bytes");
        require(f.native.wait(first_handle, 0) == (event ? 0x102u : 0u),
                "named object has the actual requested initial native state");
        require(f.create(event, output, 0) == 0 &&
                    f.memory.load<uint32_t>(output) == first_handle + 4 && f.native.open_count() == 2,
                "null attributes retain independent unnamed creation");
    }
}

void same_name_reopens_shared_state_without_reinitializing() {
    {
        Fixture f;
        require(f.guest.create_event(output, attributes, 1, 0) == 0, "create named auto-reset event");
        const auto first = f.memory.load<uint32_t>(output);
        require(f.guest.create_event(output, attributes, 0, 1) == exists, "same event name reports name exists");
        const auto second = f.memory.load<uint32_t>(output);
        require(first == second && f.native.owns(first) && f.native.open_count() == 1,
                "exact-name reopen retains the same guest handle and one unique owned ID");
        require(f.native.wait(second, 0) == 0x102, "reopen must not apply its new initial state");
        require(f.native.set_event(first) == 0 && f.native.wait(second, 0) == 0 &&
                    f.native.wait(first, 0) == 0x102,
                "reopened event shares state and preserves the original auto-reset type");
        require(f.native.close(first) == 0 && f.native.set_event(second) == 0 &&
                    f.native.wait(second, 0) == 0 && f.native.owns(second),
                "first close releases only one named-open reference");
        require(f.native.close(second) == 0 && !f.native.owns(second) && f.native.open_count() == 0,
                "last close releases the shared guest handle");
        require(f.create(true, output, 0) == 0 && f.memory.load<uint32_t>(output) == first_handle + 4,
                "same-name reopen consumes no new handle ID");
    }
    {
        Fixture f;
        require(f.guest.create_semaphore(output, attributes, 1, 2) == 0, "create named semaphore");
        const auto first = f.memory.load<uint32_t>(output);
        require(f.native.wait(first, 0) == 0, "consume original count before reopening");
        require(f.guest.create_semaphore(output, attributes, 7, 7) == exists, "same semaphore name reports name exists");
        const auto second = f.memory.load<uint32_t>(output);
        require(second == first && f.native.open_count() == 1 && f.native.wait(second, 0) == 0x102,
                "semaphore reopen retains the guest handle and consumed count");
        const auto release = f.native.release_semaphore(second, 2);
        require(release.status == 0 && release.previous_count == 0 &&
                    f.native.release_semaphore(first, 1).status != 0,
                "reopened semaphore retains the original maximum count");
        require(f.native.wait(first, 0) == 0 && f.native.wait(second, 0) == 0 &&
                    f.native.wait(second, 0) == 0x102,
                "both handles consume the same real semaphore count");
    }
}

void unaudited_case_aliases_and_cross_kind_names_fail_closed() {
    for (bool event : {false, true}) {
        Fixture f;
        f.set_name("CaseName");
        require(f.create(event) == 0, "create first named kind");
        f.memory.store<uint32_t>(output, 0x11223344);
        const auto before = f.bytes();
        rejects([&] { f.create(!event); });
        require(f.bytes() == before && f.native.open_count() == 1,
                "cross-kind name collision stops without replacing or publishing an object");
        f.set_name("casename");
        const auto case_before = f.bytes();
        rejects([&] { f.create(event); });
        require(f.bytes() == case_before && f.native.open_count() == 1,
                "case-fold-equal but nonexact spelling fails closed without publication");
        f.set_name("DifferentName");
        require(f.create(!event) == 0 && f.memory.load<uint32_t>(output) == first_handle + 4 &&
                    f.native.open_count() == 2,
                "different names are independent and rejected case/kind aliases consume no ID");
    }
}

void descriptor_values_and_unsupported_namespace_fail_before_effects() {
    for (bool event : {false, true}) {
        for (unsigned variant = 0; variant < 15; ++variant) {
            Fixture f;
            uint32_t attrs = attributes;
            switch (variant) {
            case 0: attrs += 1; break;
            case 1: f.memory.store<uint32_t>(attributes, 0); break;
            case 2: f.memory.store<uint32_t>(attributes, 0xFFFFFFFB); break;
            case 3: f.memory.store<uint32_t>(attributes + 8, 0); break;
            case 4: f.memory.store<uint32_t>(attributes + 8, 0xC0); break;
            case 5: f.memory.store<uint32_t>(attributes + 4, 0); break;
            case 6: f.memory.store<uint32_t>(attributes + 4, descriptor + 1); break;
            case 7: f.memory.store<uint16_t>(descriptor, 0); break;
            case 8: f.memory.store<uint16_t>(descriptor + 2, 1); break;
            case 9: f.memory.store<uint32_t>(descriptor + 4, 0); break;
            case 10: f.memory.store<uint32_t>(descriptor + 4, 0x40000000); break;
            case 11: f.memory.store<uint32_t>(attributes + 4, 0x40000000); break;
            case 12: attrs = 0x40000000; break;
            case 13: attrs = 0xFFFFFFFC; break;
            case 14: f.memory.store<uint32_t>(attributes + 4, 0xFFFFFFFC); break;
            }
            f.failure_without_effects(event, output, attrs);
        }
        for (uint8_t invalid : {uint8_t{0}, uint8_t{'/'}, uint8_t{'\\'}, uint8_t{1},
                                uint8_t{31}, uint8_t{127}, uint8_t{128}, uint8_t{255}}) {
            Fixture f;
            // An invalid final byte requires validation of the whole name before creation.
            f.memory.store<uint8_t>(name_bytes + 12, invalid);
            f.failure_without_effects(event);
        }
    }
}

void truncated_inputs_and_wrapping_names_are_rejected() {
    for (bool event : {false, true}) {
        for (unsigned kind = 0; kind < 3; ++kind) {
            Fixture f;
            constexpr uint32_t short_region = 0x20000000;
            const unsigned length = kind == 0 ? 11 : kind == 1 ? 7 : 12;
            f.memory.map(short_region, length);
            std::fill_n(f.memory.base() + short_region, length, uint8_t{0xDA});
            uint32_t attrs = attributes;
            if (kind == 0) {
                std::copy_n(f.memory.base() + attributes, length, f.memory.base() + short_region);
                attrs = short_region;
            } else if (kind == 1) {
                std::copy_n(f.memory.base() + descriptor, length, f.memory.base() + short_region);
                f.memory.store<uint32_t>(attributes + 4, short_region);
            } else f.memory.store<uint32_t>(descriptor + 4, short_region);
            const std::vector<uint8_t> before(f.memory.base() + short_region,
                                             f.memory.base() + short_region + length);
            f.failure_without_effects(event, output, attrs);
            require(std::equal(before.begin(), before.end(), f.memory.base() + short_region),
                    "truncated input bytes must remain unchanged");
        }
        Fixture f;
        f.memory.map(0xFFFF0000, 0x10000);
        f.memory.store<uint8_t>(0xFFFFFFFF, 'X');
        f.memory.store<uint16_t>(descriptor, 2);
        f.memory.store<uint16_t>(descriptor + 2, 2);
        f.memory.store<uint32_t>(descriptor + 4, 0xFFFFFFFF);
        f.failure_without_effects(event);
    }
}

void output_preflight_precedes_input_reads_and_object_creation() {
    for (bool event : {false, true}) {
        for (uint32_t bad_output : {0u, output + 1, region + extent, 0x40000000u}) {
            Fixture f;
            unsigned reads = 0;
            f.memory.add_read_only_word(attributes, [&] { ++reads; return 0xFFFFFFFCu; });
            f.failure_without_effects(event, bad_output);
            require(reads == 0, "invalid output must stop before sampling attribute providers");
        }
        {
            Fixture f;
            f.memory.map(0x20000000, 3);
            std::fill_n(f.memory.base() + 0x20000000, 3, uint8_t{0xB7});
            f.failure_without_effects(event, 0x20000000);
            require(f.memory.base()[0x20000000] == 0xB7 && f.memory.base()[0x20000002] == 0xB7,
                    "partially mapped output has no partial handle store");
        }
        for (bool imported : {false, true}) {
            Fixture f;
            unsigned reads = 0;
            if (imported) f.memory.add_import_variable(output, "NamedSyncOutput");
            else f.memory.add_read_only_word(output, [&] { ++reads; return 0u; });
            const auto before = f.bytes();
            rejects([&] { f.create(event); });
            require(f.bytes() == before && reads == 0 && f.native.open_count() == 0,
                    "protected output prevents creation, writes and provider sampling");
            require(f.create(event, output + 8, 0) == 0 &&
                        f.memory.load<uint32_t>(output + 8) == first_handle,
                    "protected output does not consume a handle ID");
        }
        Fixture f;
        const auto reserved = f.memory.load_reserved_word(region);
        const auto before = f.bytes();
        rejects([&] { f.create(event); });
        require(f.bytes() == before && f.native.open_count() == 0 && f.memory.has_reservation() &&
                    f.memory.store_conditional_word(region, reserved),
                "named creation preserves a live atomic reservation and has no effects");
        require(f.create(event, output, 0) == 0 && f.memory.load<uint32_t>(output) == first_handle,
                "live-reservation rejection does not consume an ID");
    }
}

void input_snapshot_precedes_overlapping_output_publication() {
    for (bool event : {false, true}) {
        for (uint32_t alias : {attributes, attributes + 4, attributes + 8,
                               descriptor, descriptor + 4, name_bytes, name_bytes + 4}) {
            Fixture f;
            auto expected = f.bytes();
            expected_handle(expected, alias, first_handle);
            require(f.create(event, alias) == 0 && f.bytes() == expected,
                    "output may overlap attributes, descriptor or name after complete input snapshot");
            // Restore all input bytes; the created native name must be an owned snapshot.
            f.set_name("GuestNamedSync");
            require(f.create(event) == exists && f.memory.load<uint32_t>(output) == first_handle &&
                        f.native.open_count() == 1,
                    "overlapping publication must not corrupt the stored native object name");
        }
    }
}

void counted_names_do_not_probe_terminators_or_maximum_length() {
    for (bool event : {false, true}) {
        Fixture f;
        constexpr uint32_t exact_name = 0x20000000;
        f.memory.map(exact_name, 1);
        f.set_name("X", exact_name);
        f.memory.store<uint16_t>(descriptor + 2, 0xFFFF);
        require(f.create(event) == 0, "one-byte name succeeds without a terminator or MaximumLength mapping");
        f.set_name("X", name_bytes + 1);
        // Name bytes need no alignment, unlike the two fixed guest descriptors.
        require(f.create(event) == exists, "unaligned counted name refers to the same object");
        f.set_name("Space :.!~");
        require(f.create(event) == 0, "supported flat printable ASCII punctuation is preserved");
    }
    {
        Fixture f;
        f.memory.map(0xFFFF0000, 0x10000);
        f.set_name("Z", 0xFFFFFFFF);
        require(f.create(true) == 0, "one-byte name may end at the last guest address");
    }
    {
        Fixture f;
        constexpr uint32_t long_name = 0x20000000;
        f.memory.map(long_name, 0xFFFF);
        f.set_name(std::string(0xFFFF, 'L'), long_name);
        require(f.create(false) == 0, "full nonzero uint16 counted-name length is accepted");
    }
}
}

int main() {
    unsigned failures = 0;
    for (auto test : {named_creation_writes_exact_be_handle_and_unnamed_still_works,
                      same_name_reopens_shared_state_without_reinitializing,
                      unaudited_case_aliases_and_cross_kind_names_fail_closed,
                      descriptor_values_and_unsupported_namespace_fail_before_effects,
                      truncated_inputs_and_wrapping_names_are_rejected,
                      output_preflight_precedes_input_reads_and_object_creation,
                      input_snapshot_precedes_overlapping_output_publication,
                      counted_names_do_not_probe_terminators_or_maximum_length}) {
        try { test(); }
        catch (const std::exception& error) { std::cerr << error.what() << '\n'; ++failures; }
    }
    return failures ? 1 : 0;
}
