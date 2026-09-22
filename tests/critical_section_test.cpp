#include "critical_section.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class F> bool rejects(F operation) {
    try { operation(); } catch (const sfr::RuntimeStop&) { return true; }
    return false;
}

std::array<uint8_t, 28> bytes_at(const sfr::GuestMemory& memory, uint32_t address) {
    std::array<uint8_t, 28> result{};
    for (uint32_t i = 0; i < result.size(); ++i) result[i] = memory.load<uint8_t>(uint64_t(address) + i);
    return result;
}

void fills_xenia_layout_and_preserves_opaque_header_bytes() {
    sfr::GuestMemory memory;
    constexpr uint32_t base = 0x10020;
    memory.map(0x10000, 0x1000);
    memory.store<uint8_t>(base - 1, 0x5a);
    memory.store<uint8_t>(base + 28, 0xa5);
    for (uint32_t i = 0; i < 28; ++i) memory.store<uint8_t>(base + i, static_cast<uint8_t>(0x80 + i));

    sfr::initialize_critical_section(memory, base);

    const std::array<uint8_t, 28> expected{
        0x01, 0x00, 0x82, 0x83, 0x00, 0x00, 0x00, 0x00,
        0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
        0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    require(bytes_at(memory, base) == expected, "critical section bytes match the Xenia 28-byte layout");
    require(memory.load<uint32_t>(base + 4) == 0, "signal state is big endian zero");
    require(memory.load<uint32_t>(base + 16) == 0xffffffffu, "lock count is big endian minus one");
    require(memory.load<uint32_t>(base + 20) == 0, "recursion count is big endian zero");
    require(memory.load<uint32_t>(base + 24) == 0, "owner is big endian zero");
    require(memory.load<uint8_t>(base - 1) == 0x5a && memory.load<uint8_t>(base + 28) == 0xa5,
            "initializer stays inside the 28-byte object");

    sfr::initialize_critical_section(memory, base);
    require(bytes_at(memory, base) == expected, "repeated initialization is stable");
}

void initializes_across_a_page_boundary() {
    sfr::GuestMemory memory;
    constexpr uint32_t base = 0x20ff0;
    memory.map(0x20000, 0x2000);
    for (uint32_t i = 0; i < 28; ++i) memory.store<uint8_t>(base + i, 0x77);

    sfr::initialize_critical_section(memory, base);

    require(memory.load<uint8_t>(base) == 1 && memory.load<uint8_t>(base + 1) == 0,
            "cross-page header initialized");
    require(memory.load<uint32_t>(base + 16) == 0xffffffffu && memory.load<uint32_t>(base + 24) == 0,
            "cross-page fields initialized");
    require(memory.load<uint8_t>(base + 8) == 0x77 && memory.load<uint8_t>(base + 15) == 0x77,
            "cross-page opaque bytes preserved");
}

void invalid_ranges_do_not_partially_initialize() {
    {
        sfr::GuestMemory memory;
        constexpr uint32_t base = 0x30ff0;
        memory.map(0x30000, 0x1000);
        for (uint32_t i = 0; i < 16; ++i) memory.store<uint8_t>(base + i, static_cast<uint8_t>(0x40 + i));
        std::array<uint8_t, 16> before{};
        for (uint32_t i = 0; i < before.size(); ++i) before[i] = memory.load<uint8_t>(base + i);
        require(rejects([&] { sfr::initialize_critical_section(memory, base); }), "unmapped tail rejected");
        for (uint32_t i = 0; i < before.size(); ++i)
            require(memory.load<uint8_t>(base + i) == before[i], "unmapped tail leaves mapped prefix unchanged");
    }
    {
        sfr::GuestMemory memory;
        constexpr uint32_t base = 0x40020;
        memory.map(0x40000, 0x1000);
        for (uint32_t i = 0; i < 28; ++i) memory.store<uint8_t>(base + i, static_cast<uint8_t>(0x20 + i));
        const auto before = bytes_at(memory, base);
        memory.add_import_variable(base + 8, "GuardedCriticalSectionOpaqueField");
        require(rejects([&] { sfr::initialize_critical_section(memory, base); }), "guarded opaque field rejected");
        for (uint32_t i = 0; i < before.size(); ++i)
            require(memory.base()[base + i] == before[i], "guarded field leaves whole object unchanged");
    }
    {
        sfr::GuestMemory memory;
        constexpr uint32_t base = 0xfffffff0u;
        memory.map(0xfffff000u, 0x1000);
        for (uint32_t i = 0; i < 16; ++i) memory.store<uint8_t>(uint64_t(base) + i, static_cast<uint8_t>(0x60 + i));
        std::array<uint8_t, 16> before{};
        for (uint32_t i = 0; i < before.size(); ++i) before[i] = memory.load<uint8_t>(uint64_t(base) + i);
        require(rejects([&] { sfr::initialize_critical_section(memory, base); }), "32-bit address overflow rejected");
        for (uint32_t i = 0; i < before.size(); ++i)
            require(memory.load<uint8_t>(uint64_t(base) + i) == before[i], "overflow leaves mapped prefix unchanged");
    }
    {
        sfr::GuestMemory memory;
        require(rejects([&] { sfr::initialize_critical_section(memory, 0x50000); }), "fully unmapped object rejected");
    }
}

void initializes_spin_count_in_xenia_units() {
    constexpr std::array<std::array<uint32_t, 2>, 9> cases{{
        {0, 0}, {1, 1}, {255, 1}, {256, 1}, {257, 2}, {4000, 16},
        {65280, 255}, {65281, 255}, {0xffffff00u, 255},
    }};
    for (uint32_t case_index = 0; case_index < cases.size(); ++case_index) {
        sfr::GuestMemory memory;
        const uint32_t base = 0x51020 + case_index * 0x40;
        memory.map(0x51000, 0x1000);
        for (uint32_t i = 0; i < 28; ++i)
            memory.store<uint8_t>(base + i, static_cast<uint8_t>(0x30 + i));
        memory.store<uint8_t>(base - 1, 0x5a);
        memory.store<uint8_t>(base + 28, 0xa5);

        require(sfr::initialize_critical_section_and_spin_count(memory, base, cases[case_index][0]) == 0,
                "spin initializer returns X_STATUS_SUCCESS");
        const std::array<uint8_t, 28> expected{
            0x01, static_cast<uint8_t>(cases[case_index][1]), 0x32, 0x33,
            0x00, 0x00, 0x00, 0x00,
            0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
            0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,
        };
        require(bytes_at(memory, base) == expected, "spin initializer writes exact required bytes");
        require(memory.load<uint8_t>(base - 1) == 0x5a && memory.load<uint8_t>(base + 28) == 0xa5,
                "spin initializer preserves neighbors");
    }
}

void invalid_spin_initialization_never_writes() {
    for (uint32_t spin_count : {0xffffff01u, 0xffffffffu}) {
        sfr::GuestMemory memory;
        constexpr uint32_t base = 0x52020;
        memory.map(0x52000, 0x1000);
        for (uint32_t i = 0; i < 28; ++i) memory.store<uint8_t>(base + i, 0x66);
        const auto before = bytes_at(memory, base);
        try {
            sfr::initialize_critical_section_and_spin_count(memory, base, spin_count);
            require(false, "overflowing spin count rejected");
        } catch (const sfr::RuntimeStop& stop) {
            require(stop.category == "critical-section-spin", "overflow has spin diagnostic category");
            require(stop.address == base, "overflow identifies critical section address");
        }
        require(bytes_at(memory, base) == before, "overflowing spin count leaves object unchanged");
    }

    for (uint32_t base : {0x530ff0u, 0xfffffff0u}) {
        sfr::GuestMemory memory;
        memory.map(base & ~0xfffu, 0x1000);
        for (uint32_t i = 0; i < 16; ++i)
            memory.store<uint8_t>(uint64_t(base) + i, static_cast<uint8_t>(0x70 + i));
        std::array<uint8_t, 16> before{};
        for (uint32_t i = 0; i < before.size(); ++i)
            before[i] = memory.load<uint8_t>(uint64_t(base) + i);
        require(rejects([&] { sfr::initialize_critical_section_and_spin_count(memory, base, 4000); }),
                "spin initializer rejects missing or overflowing tail");
        for (uint32_t i = 0; i < before.size(); ++i)
            require(memory.load<uint8_t>(uint64_t(base) + i) == before[i],
                    "invalid spin object preserves mapped prefix");
    }

    for (uint32_t guard_offset : {12u, 24u}) {
        sfr::GuestMemory memory;
        constexpr uint32_t base = 0x54020;
        memory.map(0x54000, 0x1000);
        for (uint32_t i = 0; i < 28; ++i) memory.store<uint8_t>(base + i, 0x55);
        const auto before = bytes_at(memory, base);
        memory.add_import_variable(base + guard_offset, "GuardedSpinCriticalSection");
        require(rejects([&] { sfr::initialize_critical_section_and_spin_count(memory, base, 4000); }),
                "spin initializer rejects guarded object");
        for (uint32_t i = 0; i < before.size(); ++i)
            require(memory.base()[base + i] == before[i], "guarded spin object remains unchanged");
    }
}

void spin_byte_survives_lock_transitions() {
    sfr::GuestMemory memory;
    constexpr uint32_t base = 0x55020;
    constexpr uint32_t owner = 0x70000bb0;
    memory.map(0x55000, 0x1000);
    sfr::initialize_critical_section_and_spin_count(memory, base, 4000);
    sfr::enter_critical_section(memory, base, owner);
    require(memory.load<uint8_t>(base + 1) == 16, "enter preserves initialized spin byte");
    sfr::leave_critical_section(memory, base, owner);
    require(memory.load<uint8_t>(base + 1) == 16, "leave preserves initialized spin byte");
}

constexpr uint32_t thread = 0x70000bb0;

void set_state(sfr::GuestMemory& memory, uint32_t base, uint32_t lock, uint32_t recursion,
               uint32_t owner) {
    memory.store<uint32_t>(base + 16, lock);
    memory.store<uint32_t>(base + 20, recursion);
    memory.store<uint32_t>(base + 24, owner);
}

void require_state(const sfr::GuestMemory& memory, uint32_t base, uint32_t lock,
                   uint32_t recursion, uint32_t owner, const char* message) {
    require(memory.load<uint32_t>(base + 16) == lock &&
            memory.load<uint32_t>(base + 20) == recursion &&
            memory.load<uint32_t>(base + 24) == owner, message);
}

template<class F> void rejects_unchanged(sfr::GuestMemory& memory, uint32_t base,
                                       F operation, const char* message) {
    // Test-only raw inspection includes guarded bytes; production must use checked accesses.
    std::array<uint8_t, 28> before{};
    for (uint32_t i = 0; i < before.size(); ++i) before[i] = memory.base()[uint64_t(base) + i];
    require(rejects(operation), message);
    for (uint32_t i = 0; i < before.size(); ++i)
        require(memory.base()[uint64_t(base) + i] == before[i], "rejected operation changed object bytes");
}

void acquires_recurses_releases_and_reacquires_static_lock() {
    sfr::GuestMemory memory;
    constexpr uint32_t base = 0x82ad08f0;
    memory.map(0x82ad0000, 0x1000);
    // The title's static lock, including its spin byte and intrusive list links.
    memory.store<uint32_t>(base, 0x01000400);
    memory.store<uint32_t>(base + 4, 0);
    memory.store<uint32_t>(base + 8, base + 8);
    memory.store<uint32_t>(base + 12, base + 8);
    set_state(memory, base, 0xffffffffu, 0, 0);
    memory.store<uint8_t>(base - 1, 0xa5);
    memory.store<uint8_t>(base + 28, 0x5a);
    const auto before = bytes_at(memory, base);

    sfr::enter_critical_section(memory, base, thread);
    require_state(memory, base, 0, 1, thread, "first acquisition must install owner and counts");
    sfr::enter_critical_section(memory, base, thread);
    const std::array<uint8_t, 28> twice{
        0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x82, 0xad, 0x08, 0xf8, 0x82, 0xad, 0x08, 0xf8,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02,
        0x70, 0x00, 0x0b, 0xb0,
    };
    require(bytes_at(memory, base) == twice, "recursive acquisition uses exact big-endian bytes");
    sfr::leave_critical_section(memory, base, thread);
    require_state(memory, base, 0, 1, thread, "recursive leave retains owner");
    sfr::leave_critical_section(memory, base, thread);
    require(bytes_at(memory, base) == before, "final leave restores free state and preserves header");
    sfr::enter_critical_section(memory, base, thread + 4);
    require_state(memory, base, 0, 1, thread + 4, "released lock can be acquired by another owner");
    sfr::leave_critical_section(memory, base, thread + 4);
    require(bytes_at(memory, base) == before, "reacquired lock releases cleanly");
    require(memory.load<uint8_t>(base - 1) == 0xa5 && memory.load<uint8_t>(base + 28) == 0x5a,
            "transitions do not touch neighbors");
}

void independent_locks_and_cross_page_transitions() {
    sfr::GuestMemory memory;
    constexpr uint32_t first = 0x60ff0, second = 0x61040;
    memory.map(0x60000, 0x2000);
    sfr::initialize_critical_section(memory, first);
    sfr::initialize_critical_section(memory, second);
    sfr::enter_critical_section(memory, first, thread);
    sfr::enter_critical_section(memory, second, thread);
    sfr::enter_critical_section(memory, first, thread);
    sfr::leave_critical_section(memory, second, thread);
    require_state(memory, first, 1, 2, thread, "independent second lock leaves first unchanged");
    require_state(memory, second, 0xffffffffu, 0, 0, "second lock independently released");
    sfr::leave_critical_section(memory, first, thread);
    sfr::leave_critical_section(memory, first, thread);
}

void invalid_states_and_owners_never_write() {
    sfr::GuestMemory memory;
    constexpr uint32_t base = 0x70020;
    memory.map(0x70000, 0x1000);
    sfr::initialize_critical_section(memory, base);
    // Include waiter counts, negative counts, invalid recursion, and inconsistent owners.
    const std::array<std::array<uint32_t, 3>, 11> states{{
        {0, 0, 0}, {0xffffffffu, 1, thread}, {0xffffffffu, 0, thread},
        {0, 1, 0}, {1, 1, thread}, {0, 2, thread}, {2, 1, 0},
        {0xfffffffeu, 0, 0}, {0x7fffffffu, 0x80000000u, thread},
        {0xfffffffeu, 0xffffffffu, thread}, {0, 0, thread},
    }};
    for (const auto& state : states) {
        set_state(memory, base, state[0], state[1], state[2]);
        rejects_unchanged(memory, base, [&] { sfr::enter_critical_section(memory, base, thread); },
                          "enter rejects inconsistent state");
        rejects_unchanged(memory, base, [&] { sfr::leave_critical_section(memory, base, thread); },
                          "leave rejects inconsistent state");
    }
    set_state(memory, base, 0xffffffffu, 0, 0);
    rejects_unchanged(memory, base, [&] { sfr::leave_critical_section(memory, base, thread); },
                      "leave rejects free lock");
    set_state(memory, base, 0, 1, thread);
    rejects_unchanged(memory, base, [&] { sfr::leave_critical_section(memory, base, thread + 4); },
                      "leave rejects foreign owner");
    rejects_unchanged(memory, base, [&] {
        try { sfr::enter_critical_section(memory, base, thread + 4); }
        catch (const sfr::RuntimeStop& stop) {
            require(stop.category == "critical-section-contention", "foreign enter has contention category");
            require(stop.address == base, "contention identifies lock address");
            throw;
        }
    }, "enter rejects foreign owner instead of waiting");
}

void validates_header_alignment_and_thread_before_writing() {
    sfr::GuestMemory memory;
    constexpr uint32_t base = 0x80020;
    memory.map(0x80000, 0x1000);
    for (bool entering : {true, false}) {
        auto operation = [&](uint32_t address, uint32_t owner) {
            if (entering) sfr::enter_critical_section(memory, address, owner);
            else sfr::leave_critical_section(memory, address, owner);
        };
        for (uint32_t offset = 1; offset <= 3; ++offset) {
            sfr::initialize_critical_section(memory, base + offset);
            if (!entering) set_state(memory, base + offset, 0, 1, thread);
            rejects_unchanged(memory, base + offset, [&] { operation(base + offset, thread); },
                              "unaligned lock rejected");
        }
        sfr::initialize_critical_section(memory, base);
        if (!entering) set_state(memory, base, 0, 1, thread);
        rejects_unchanged(memory, base, [&] { operation(base, 0); }, "zero thread rejected");
        memory.store<uint8_t>(base, 0);
        rejects_unchanged(memory, base, [&] { operation(base, thread); }, "wrong event type rejected");
        memory.store<uint8_t>(base, 1);
        memory.store<uint32_t>(base + 4, 1);
        rejects_unchanged(memory, base, [&] { operation(base, thread); }, "signaled event rejected");
    }
}

void recursion_boundary_is_checked_without_overflow() {
    sfr::GuestMemory memory;
    constexpr uint32_t base = 0x90020;
    memory.map(0x90000, 0x1000);
    sfr::initialize_critical_section(memory, base);
    set_state(memory, base, 0x7ffffffdu, 0x7ffffffeu, thread);
    sfr::enter_critical_section(memory, base, thread);
    require_state(memory, base, 0x7ffffffeu, 0x7fffffffu, thread, "recursion can reach signed maximum");
    rejects_unchanged(memory, base, [&] { sfr::enter_critical_section(memory, base, thread); },
                      "recursion overflow rejected");
    sfr::leave_critical_section(memory, base, thread);
    require_state(memory, base, 0x7ffffffdu, 0x7ffffffeu, thread, "maximum recursion can leave");
}

void incomplete_or_guarded_objects_never_write() {
    for (bool entering : {true, false}) {
        auto operation = [&](sfr::GuestMemory& memory, uint32_t base) {
            if (entering) sfr::enter_critical_section(memory, base, thread);
            else sfr::leave_critical_section(memory, base, thread);
        };
        for (uint32_t base : {0xa0fe8u, 0xffffffe8u}) {
            sfr::GuestMemory memory;
            memory.map(base & ~0xfffu, 0x1000);
            for (uint32_t i = 0; i < 24; ++i) memory.store<uint8_t>(uint64_t(base) + i, 0x77);
            memory.store<uint32_t>(base, 0x01000400);
            memory.store<uint32_t>(base + 4, 0);
            memory.store<uint32_t>(base + 16, entering ? 0xffffffffu : 0);
            memory.store<uint32_t>(base + 20, entering ? 0 : 1);
            std::array<uint8_t, 24> before{};
            for (uint32_t i = 0; i < before.size(); ++i) before[i] = memory.load<uint8_t>(uint64_t(base) + i);
            require(rejects([&] { operation(memory, base); }), "missing tail or address overflow rejected");
            for (uint32_t i = 0; i < before.size(); ++i)
                require(memory.load<uint8_t>(uint64_t(base) + i) == before[i], "invalid tail preserves mapped prefix");
        }
        for (uint32_t guard_offset : {8u, 24u}) {
            sfr::GuestMemory memory;
            constexpr uint32_t base = 0xb0020;
            memory.map(0xb0000, 0x1000);
            sfr::initialize_critical_section(memory, base);
            if (!entering) set_state(memory, base, 0, 1, thread);
            memory.add_import_variable(base + guard_offset, "GuardedCriticalSectionField");
            rejects_unchanged(memory, base, [&] { operation(memory, base); }, "guarded object rejected");
        }
        sfr::GuestMemory memory;
        require(rejects([&] { operation(memory, 0xc0000); }), "fully unmapped transition rejected");
    }
}

void readonly_tail_rejects_multiwrite_operations_before_changes() {
    for (uint32_t operation = 0; operation < 4; ++operation) {
        sfr::GuestMemory memory;
        constexpr uint32_t base = 0xd0020;
        memory.map(0xd0000, 0x1000);
        sfr::initialize_critical_section(memory, base);
        if (operation < 2) memory.store<uint32_t>(base, 0x55667788);
        if (operation == 3) sfr::enter_critical_section(memory, base, thread);
        unsigned samples = 0;
        const uint32_t owner = operation == 3 ? thread : 0;
        memory.add_read_only_word(base + 24, [&] { ++samples; return owner; });
        rejects_unchanged(memory, base, [&] {
            if (operation == 0) sfr::initialize_critical_section(memory, base);
            else if (operation == 1) sfr::initialize_critical_section_and_spin_count(memory, base, 4000);
            else if (operation == 2) sfr::enter_critical_section(memory, base, thread);
            else sfr::leave_critical_section(memory, base, thread);
        }, "readonly owner rejects whole critical section operation");
        require(samples == 0, "failed critical section operation never samples readonly provider");
    }
}
}

int main() {
    try {
        fills_xenia_layout_and_preserves_opaque_header_bytes();
        initializes_across_a_page_boundary();
        invalid_ranges_do_not_partially_initialize();
        initializes_spin_count_in_xenia_units();
        invalid_spin_initialization_never_writes();
        spin_byte_survives_lock_transitions();
        acquires_recurses_releases_and_reacquires_static_lock();
        independent_locks_and_cross_page_transitions();
        invalid_states_and_owners_never_write();
        validates_header_alignment_and_thread_before_writing();
        recursion_boundary_is_checked_without_overflow();
        incomplete_or_guarded_objects_never_write();
        readonly_tail_rejects_multiwrite_operations_before_changes();
        std::cout << "Critical section checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
