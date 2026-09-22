#include "guest_critical_sections.h"
#include "critical_section.h"
#include "guest_execution.h"
#include "guest_memory.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <future>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <stop_token>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <malloc.h>
#endif

namespace allocation_failure {
// Only the current test thread's explicitly scoped enter() call is instrumented.
thread_local int countdown = -1;
void check() {
    if (countdown < 0) return;
    if (countdown-- == 0) {
        countdown = -1; // Cleanup can allocate normally after the injected failure.
        throw std::bad_alloc();
    }
}
struct Scope {
    explicit Scope(int index) { countdown = index; }
    ~Scope() { countdown = -1; }
};
}

void* operator new(std::size_t size) {
    allocation_failure::check();
    if (void* result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void* operator new(std::size_t size, std::align_val_t alignment) {
    allocation_failure::check();
    void* result = nullptr;
#ifdef _WIN32
    result = _aligned_malloc(size ? size : 1, static_cast<std::size_t>(alignment));
#else
    if (posix_memalign(&result, static_cast<std::size_t>(alignment), size ? size : 1) != 0)
        result = nullptr;
#endif
    if (result) return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}
void operator delete(void* pointer, std::align_val_t) noexcept {
#ifdef _WIN32
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}
void operator delete[](void* pointer, std::align_val_t alignment) noexcept {
    ::operator delete(pointer, alignment);
}
void operator delete(void* pointer, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(pointer, alignment);
}
void operator delete[](void* pointer, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(pointer, alignment);
}

namespace {
using namespace std::chrono_literals;
constexpr uint32_t section = 0xFFCFFFD4, second_section = section - 64;
constexpr uint32_t owner_thread = 0x73201000, main_thread = 0x70000BB0;
constexpr uint32_t other_thread = 0x73202000, late_thread = 0x73203000;
using Bytes = std::array<uint8_t, 30>;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void rejects(F operation) {
    try { operation(); } catch (const sfr::RuntimeStop&) { return; }
    throw std::runtime_error("invalid critical-section operation did not stop");
}
template<class T> T ready(std::future<T>& result, const char* message) {
    require(result.wait_for(2s) == std::future_status::ready, message);
    return result.get();
}
struct Fixture {
    sfr::GuestMemory memory;
    sfr::GuestCriticalSections locks{memory};
    Fixture() {
        memory.map(0xFFCF0000, 0x10000);
        initialize(section);
        initialize(second_section);
    }
    void initialize(uint32_t address) {
        for (unsigned i = 0; i < 30; ++i)
            memory.store<uint8_t>(uint64_t(address) - 1 + i, static_cast<uint8_t>(0x80 + i));
        sfr::initialize_critical_section_and_spin_count(memory, address, 4096);
    }
    Bytes bytes(uint32_t address = section) const {
        Bytes result{};
        std::copy_n(memory.base() + address - 1, result.size(), result.begin());
        return result;
    }
    void state(uint32_t lock, uint32_t recursion, uint32_t owner, uint32_t address = section) const {
        require(memory.load<uint32_t>(uint64_t(address) + 16) == lock &&
                    memory.load<uint32_t>(uint64_t(address) + 20) == recursion &&
                    memory.load<uint32_t>(uint64_t(address) + 24) == owner,
                "critical-section counter/recursion/owner must match the exact transition");
        require(memory.load<uint32_t>(uint64_t(address) + 4) == 0,
                "handoff consumes its auto-reset event signal without a stale guest signal bit");
    }
    void opaque_unchanged(const Bytes& before, uint32_t address = section) const {
        const auto after = bytes(address);
        require(std::equal(before.begin(), before.begin() + 17, after.begin()) &&
                    before.back() == after.back(),
                "opaque header/list/spin bytes and object neighbors must be preserved");
    }
};
struct StopLocks {
    sfr::GuestCriticalSections& locks;
    ~StopLocks() { locks.stop(); }
};
struct StopToken {
    std::stop_source& source;
    ~StopToken() { source.request_stop(); }
};
struct StopExecution {
    sfr::GuestExecution& execution;
    sfr::GuestCriticalSections& locks;
    ~StopExecution() { execution.stop(); locks.stop(); }
};

void observed_contention_has_registration_before_park_and_exact_handoff() {
    Fixture f;
    const auto original = f.bytes();
    require(!f.locks.enter(section, owner_thread), "observed worker acquires free section immediately");
    f.state(0, 1, owner_thread);
    auto main = f.locks.enter(section, main_thread);
    require(bool(main), "observed main thread receives a pending contention token");
    f.state(1, 1, owner_thread);
    const auto pending = f.bytes();
    rejects([&] { f.locks.complete(*main); });
    require(f.bytes() == pending, "completion before selection has no guest effects");
    f.locks.leave(section, owner_thread);
    f.state(0, 0, 0);
    // Final leave deliberately happens before wait starts: readiness must persist.
    auto woke = std::async(std::launch::async, [&] { return main->wait({}); });
    StopLocks cleanup{f.locks};
    require(ready(woke, "wake before actual wait must not be lost"), "selected token reports actual readiness");
    f.locks.complete(*main);
    f.state(0, 1, main_thread);
    const auto acquired = f.bytes();
    rejects([&] { f.locks.complete(*main); });
    require(f.bytes() == acquired, "a token cannot finalize ownership twice");
    f.locks.leave(section, main_thread);
    f.state(0xFFFFFFFF, 0, 0);
    f.opaque_unchanged(original);
}

void recursion_multiple_waiters_and_handoff_prevent_barging() {
    Fixture f;
    const auto original = f.bytes();
    require(!f.locks.enter(section, owner_thread) && !f.locks.enter(section, owner_thread),
            "owner recursion acquires without a waiter");
    auto first = f.locks.enter(section, main_thread);
    auto second = f.locks.enter(section, other_thread);
    require(first && second, "two other threads are admitted as waiters");
    f.state(3, 2, owner_thread);
    f.locks.leave(section, owner_thread);
    f.state(2, 1, owner_thread);
    const auto recursive = f.bytes();
    rejects([&] { f.locks.complete(*first); });
    rejects([&] { f.locks.complete(*second); });
    require(f.bytes() == recursive, "recursive leave does not select a waiter");
    f.locks.leave(section, owner_thread);
    f.state(1, 0, 0);
    auto late = f.locks.enter(section, late_thread);
    require(bool(late), "new arrival cannot acquire owner-zero handoff state");
    f.state(2, 0, 0);
    auto first_wake = std::async(std::launch::async, [&] { return first->wait({}); });
    auto second_wake = std::async(std::launch::async, [&] { return second->wait({}); });
    auto late_wake = std::async(std::launch::async, [&] { return late->wait({}); });
    StopLocks cleanup{f.locks};
    require(ready(first_wake, "first admitted waiter must receive handoff"), "first waiter is selected");
    require(second_wake.wait_for(20ms) == std::future_status::timeout &&
                late_wake.wait_for(20ms) == std::future_status::timeout,
            "one final leave wakes exactly one waiter");
    rejects([&] { f.locks.complete(*second); });
    rejects([&] { f.locks.complete(*late); });
    f.locks.complete(*first);
    f.state(2, 1, main_thread);
    require(!f.locks.enter(section, main_thread), "selected owner can recurse while waiters remain");
    f.state(3, 2, main_thread);
    f.locks.leave(section, main_thread);
    f.state(2, 1, main_thread);
    f.locks.leave(section, main_thread);
    require(ready(second_wake, "second admitted waiter receives next handoff"), "second waiter selected");
    f.state(1, 0, 0);
    f.locks.complete(*second);
    f.state(1, 1, other_thread);
    f.locks.leave(section, other_thread);
    require(ready(late_wake, "late arrival waits until both earlier contenders finish"), "late waiter selected");
    f.locks.complete(*late);
    f.state(0, 1, late_thread);
    f.locks.leave(section, late_thread);
    f.state(0xFFFFFFFF, 0, 0);
    f.opaque_unchanged(original);
}

void independent_addresses_and_foreign_tokens_cannot_cross_handoff() {
    Fixture f;
    require(!f.locks.enter(section, owner_thread) && !f.locks.enter(second_section, other_thread),
            "independent sections have independent owners");
    auto first = f.locks.enter(section, main_thread);
    auto second = f.locks.enter(second_section, late_thread);
    require(first && second, "independent section waiters are admitted");
    auto second_wake = std::async(std::launch::async, [&] { return second->wait({}); });
    StopLocks cleanup{f.locks};
    f.locks.leave(section, owner_thread);
    f.state(1, 1, other_thread, second_section);
    require(second_wake.wait_for(20ms) == std::future_status::timeout,
            "leaving one address never wakes another address's waiter");
    sfr::GuestCriticalSections foreign(f.memory);
    const auto before = f.bytes();
    rejects([&] { foreign.complete(*first); });
    require(f.bytes() == before, "foreign coordinator cannot consume selected ownership");
    f.locks.complete(*first);
    f.locks.leave(section, main_thread);
    f.locks.leave(second_section, other_thread);
    require(ready(second_wake, "second address wakes after its own final leave"), "second address selected");
    f.locks.complete(*second);
    f.locks.leave(second_section, late_thread);
    f.state(0xFFFFFFFF, 0, 0);
    f.state(0xFFFFFFFF, 0, 0, second_section);
}

void invalid_states_and_late_guards_have_no_partial_effects() {
    for (unsigned variant = 0; variant < 8; ++variant) {
        Fixture f;
        switch (variant) {
        case 0: f.memory.store<uint8_t>(section, 0); break;
        case 1: f.memory.store<uint32_t>(section + 4, 1); break;
        case 2: f.memory.store<uint32_t>(section + 16, 0); break;
        case 3: f.memory.store<uint32_t>(section + 20, 1); break;
        case 4: f.memory.store<uint32_t>(section + 24, owner_thread); break;
        case 5:
            f.memory.store<uint32_t>(section + 16, 1);
            f.memory.store<uint32_t>(section + 20, 1);
            f.memory.store<uint32_t>(section + 24, owner_thread);
            break; // Guest claims a waiter which the coordinator never admitted.
        case 6:
            f.memory.store<uint32_t>(section + 16, 0x7FFFFFFE);
            f.memory.store<uint32_t>(section + 20, 0x7FFFFFFF);
            f.memory.store<uint32_t>(section + 24, owner_thread);
            break;
        case 7: f.memory.store<uint32_t>(section + 20, 0x80000000); break;
        }
        const auto before = f.bytes();
        rejects([&] { (void)f.locks.enter(section, owner_thread); });
        require(f.bytes() == before, "corrupt/unrepresented/overflow state fails before mutation");
    }
    for (bool imported : {false, true}) {
        for (uint32_t offset : {8u, 24u}) {
            Fixture f;
            require(!f.locks.enter(section, owner_thread), "guard fixture acquires before protecting bytes");
            unsigned samples = 0;
            if (imported) f.memory.add_import_variable(section + offset, "CriticalSectionGuard");
            else f.memory.add_read_only_word(section + offset, [&] { ++samples; return 0u; });
            const auto before = f.bytes();
            rejects([&] { (void)f.locks.enter(section, main_thread); });
            rejects([&] { f.locks.leave(section, owner_thread); });
            require(f.bytes() == before && samples == 0,
                    "full 28-byte preflight catches opaque/tail guards without sampling or effects");
        }
    }
    Fixture f;
    const auto before = f.bytes();
    rejects([&] { (void)f.locks.enter(section + 1, owner_thread); });
    rejects([&] { (void)f.locks.enter(section, 0); });
    rejects([&] { (void)f.locks.enter(0x30000000, owner_thread); });
    rejects([&] { f.locks.leave(section, owner_thread); });
    require(f.bytes() == before, "invalid address/thread or leaving an unowned lock preserves guest bytes");
    f.memory.map(0x30000000, 27);
    std::fill_n(f.memory.base() + 0x30000000, 27, uint8_t{0xA7});
    rejects([&] { (void)f.locks.enter(0x30000000, owner_thread); });
    require(std::all_of(f.memory.base() + 0x30000000, f.memory.base() + 0x3000001B,
                        [](uint8_t byte) { return byte == 0xA7; }),
            "unmapped final byte does not partially mutate a critical section");
}

void live_reservation_and_invalid_leave_do_not_admit_phantom_waiters() {
    Fixture f;
    require(!f.locks.enter(section, owner_thread), "owner acquires reservation fixture");
    const auto reserved = f.memory.load_reserved_word(second_section + 16);
    const auto before = f.bytes();
    rejects([&] { (void)f.locks.enter(section, main_thread); });
    rejects([&] { f.locks.leave(section, owner_thread); });
    require(f.bytes() == before && f.memory.has_reservation() &&
                f.memory.store_conditional_word(second_section + 16, reserved),
            "reservation failure preserves both lock bytes and reservation identity");
    rejects([&] { f.locks.leave(section, main_thread); });
    require(f.bytes() == before, "nonowner leave does not alter state");
    auto pending = f.locks.enter(section, main_thread);
    require(bool(pending), "valid admission still works after failed operations");
    f.state(1, 1, owner_thread);
    f.locks.leave(section, owner_thread);
    f.locks.complete(*pending);
    f.state(0, 1, main_thread);
    f.locks.leave(section, main_thread);
    f.state(0xFFFFFFFF, 0, 0);
}

void signed_lock_count_boundaries_reject_before_admission_or_recursion() {
    Fixture f;
    // A high recursion count reaches the signed boundary with just two waiters;
    // the test needs neither billions of admissions nor a special runtime hook.
    f.memory.store<uint32_t>(section + 16, 0x7FFFFFFD);
    f.memory.store<uint32_t>(section + 20, 0x7FFFFFFE);
    f.memory.store<uint32_t>(section + 24, owner_thread);
    auto first = f.locks.enter(section, main_thread);
    auto second = f.locks.enter(section, other_thread);
    require(first && second, "two bounded admissions reach INT32_MAX lock count");
    f.state(0x7FFFFFFF, 0x7FFFFFFE, owner_thread);
    const auto before = f.bytes();
    rejects([&] { (void)f.locks.enter(section, late_thread); });
    rejects([&] { (void)f.locks.enter(section, owner_thread); });
    require(f.bytes() == before, "signed overflow must not write counters or admit a phantom waiter");
    // Removing one recursive hold makes exactly one additional admission legal.
    // This also proves both rejected entries left the coordinator queue intact.
    f.locks.leave(section, owner_thread);
    f.state(0x7FFFFFFE, 0x7FFFFFFD, owner_thread);
    auto late = f.locks.enter(section, late_thread);
    require(bool(late), "coordinator remains usable at the boundary after rejected entries");
    f.state(0x7FFFFFFF, 0x7FFFFFFD, owner_thread);
    const auto admitted = f.bytes();
    rejects([&] { (void)f.locks.enter(section, owner_thread); });
    require(f.bytes() == admitted, "recursive increment independently respects the signed lock-count limit");
    f.locks.stop(); // Terminally release the synthetic high-recursion fixture.
}

void every_enter_allocation_failure_preserves_bytes_and_coordinator_usability() {
    for (bool contended : {false, true}) {
        unsigned injected = 0;
        bool reached_success = false;
        for (int failure_index = 0; failure_index < 16; ++failure_index) {
            Fixture f;
            const auto free_state = f.bytes();
            if (contended)
                require(!f.locks.enter(section, owner_thread), "allocation fixture starts owned");
            const auto before = f.bytes();
            std::unique_ptr<sfr::GuestCriticalSections::PendingEnter> pending;
            bool failed = false;
            try {
                allocation_failure::Scope fail_at(failure_index);
                pending = f.locks.enter(section, contended ? main_thread : owner_thread);
            } catch (const std::bad_alloc&) {
                failed = true;
                ++injected;
            }
            if (failed) {
                require(!pending && f.bytes() == before,
                        "each failed allocation rolls back before guest effects and token publication");
                // Retry on the SAME coordinator; a stopped registry, leaked queue
                // entry or unrepresented count cannot be hidden by a fresh fixture.
                pending = f.locks.enter(section, contended ? main_thread : owner_thread);
            } else reached_success = true;
            if (contended) {
                require(bool(pending), "retry or successful allocation admits exactly one real waiter");
                f.state(1, 1, owner_thread);
                f.locks.leave(section, owner_thread);
                f.locks.complete(*pending);
                f.state(0, 1, main_thread);
                f.locks.leave(section, main_thread);
            } else {
                require(!pending, "free entry needs no pending token after allocation retry");
                f.state(0, 1, owner_thread);
                f.locks.leave(section, owner_thread);
            }
            require(f.bytes() == free_state,
                    "allocation rollback leaves no phantom waiter and permits a complete ownership lifetime");
            if (reached_success) break;
        }
        require(reached_success && injected > 0,
                "allocation sweep must observe failures and exhaust all entry allocations within sixteen steps");
    }
}

void write_combined_memory_retains_protection_and_full_state() {
    // This checks how a write-combined page is handled, so ask for the
    // console's mapping (off by default, see set_host_write_combining).
    sfr::GuestMemory::set_host_write_combining(true);
    struct Restore { ~Restore() { sfr::GuestMemory::set_host_write_combining(false); } } restore;
    Fixture f;
    constexpr uint32_t wc = 0x30000020;
    f.memory.map_write_combined(0x30000000, 0x1000);
    f.initialize(wc);
    const auto original = f.bytes(wc);
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION before{}, after{};
    require(VirtualQuery(f.memory.base() + wc, &before, sizeof(before)) == sizeof(before),
            "WC mapping native protection is queryable");
#endif
    require(!f.locks.enter(wc, owner_thread), "WC critical section supports actual free acquisition");
    auto pending = f.locks.enter(wc, main_thread);
    require(bool(pending), "WC contention is admitted with checked stores");
    f.locks.leave(wc, owner_thread);
    f.locks.complete(*pending);
    f.locks.leave(wc, main_thread);
    f.state(0xFFFFFFFF, 0, 0, wc);
    f.opaque_unchanged(original, wc);
#ifdef _WIN32
    require(VirtualQuery(f.memory.base() + wc, &after, sizeof(after)) == sizeof(after) &&
                before.Protect == after.Protect && (after.Protect & PAGE_WRITECOMBINE),
            "coordinator stores preserve write-combined native page protection");
#endif
}

void terminal_cancellation_wakes_waiters_and_never_acquires() {
    for (bool selected : {false, true}) {
        Fixture f;
        require(!f.locks.enter(section, owner_thread), "cancellation fixture owner acquires");
        auto pending = f.locks.enter(section, main_thread);
        require(bool(pending), "cancellation fixture admits waiter");
        if (selected) f.locks.leave(section, owner_thread);
        const auto before = f.bytes();
        std::stop_source stop;
        stop.request_stop();
        require(!pending->wait(stop.get_token()), "already-requested stop wins even over selected readiness");
        f.locks.stop();
        rejects([&] { f.locks.complete(*pending); });
        require(f.bytes() == before, "cancelled waiter never writes itself as owner");
    }
    Fixture f;
    require(!f.locks.enter(section, owner_thread), "global cancellation owner acquires");
    auto first = f.locks.enter(section, main_thread);
    auto second = f.locks.enter(section, other_thread);
    require(first && second, "global cancellation admits two waiters");
    const auto before = f.bytes();
    auto one = std::async(std::launch::async, [&] { return first->wait({}); });
    auto two = std::async(std::launch::async, [&] { return second->wait({}); });
    StopLocks cleanup{f.locks};
    f.locks.stop();
    require(!ready(one, "coordinator stop wakes first waiter") &&
                !ready(two, "coordinator stop wakes second waiter"),
            "global stop reports cancellation to every waiter");
    require(f.bytes() == before, "host-only cancellation does not touch terminal guest state");
}

void pending_token_survives_coordinator_destruction_as_cancelled() {
    Fixture f;
    auto coordinator = std::make_unique<sfr::GuestCriticalSections>(f.memory);
    require(!coordinator->enter(section, owner_thread), "destruction fixture acquires owner");
    auto pending = coordinator->enter(section, main_thread);
    require(bool(pending), "destruction fixture holds independently owned token");
    const auto before = f.bytes();
    coordinator.reset();
    std::stop_source stop;
    auto result = std::async(std::launch::async, [&] { return pending->wait(stop.get_token()); });
    StopToken cleanup{stop};
    require(!ready(result, "destroyed coordinator must leave a safe cancelled token"),
            "waiter lifetime does not access a destroyed coordinator");
    require(f.bytes() == before, "coordinator destruction has no ungated guest mutation");
}

void initialization_preserves_existing_layout_and_rejects_active_lifetimes() {
    for (bool with_spin : {false, true}) {
        Fixture untracked;
        // RtlInitializeCriticalSection initializes storage; it must not require
        // an already valid event header or lock state at an untracked address.
        for (uint32_t address : {section, second_section})
            for (unsigned i = 0; i < 30; ++i)
                untracked.memory.store<uint8_t>(uint64_t(address) - 1 + i,
                                                static_cast<uint8_t>(0xC0 + i));
        if (with_spin) {
            sfr::initialize_critical_section_and_spin_count(untracked.memory, second_section, 4096);
            require(untracked.locks.initialize_and_spin_count(section, 4096) == 0,
                    "spin initialization accepts arbitrary untracked storage");
        } else {
            sfr::initialize_critical_section(untracked.memory, second_section);
            untracked.locks.initialize(section);
        }
        require(untracked.bytes() == untracked.bytes(second_section),
                "untracked arbitrary bytes initialize exactly like the original helper, preserving opaque bytes");
        untracked.state(0xFFFFFFFF, 0, 0);
    }
    Fixture f;
    for (uint32_t spin : {0u, 1u, 257u, 4096u, 0xFFFFFF00u}) {
        sfr::initialize_critical_section_and_spin_count(f.memory, second_section, spin);
        require(f.locks.initialize_and_spin_count(section, spin) == 0 &&
                    f.bytes() == f.bytes(second_section),
                "coordinator spin initialization preserves the existing exact layout and success status");
    }
    sfr::initialize_critical_section(f.memory, second_section);
    f.locks.initialize(section);
    require(f.bytes() == f.bytes(second_section), "plain initialization matches existing helper bytes");
    const auto initialized = f.bytes();
    rejects([&] { (void)f.locks.initialize_and_spin_count(section, 0xFFFFFF01); });
    require(f.bytes() == initialized, "invalid spin initialization preserves all bytes");
    auto refuse_reinitialization = [&] {
        const auto before = f.bytes();
        rejects([&] { f.locks.initialize(section); });
        rejects([&] { (void)f.locks.initialize_and_spin_count(section, 0); });
        require(f.bytes() == before, "initialization cannot erase active ownership, admissions or handoff");
    };
    require(!f.locks.enter(section, owner_thread), "initialization lifetime fixture owns lock");
    refuse_reinitialization();
    auto pending = f.locks.enter(section, main_thread);
    require(bool(pending), "initialization lifetime fixture admits waiter");
    refuse_reinitialization();
    f.locks.leave(section, owner_thread);
    refuse_reinitialization();
    f.locks.complete(*pending);
    f.state(0, 1, main_thread);
    refuse_reinitialization();
    f.locks.leave(section, main_thread);
    f.locks.initialize(section);
    f.state(0xFFFFFFFF, 0, 0);
    require(f.bytes() == initialized, "initialization works again after the completed ownership lifetime");
}

void abandoning_uncompleted_admission_stops_peers_without_guest_mutation() {
    for (bool selected : {false, true}) {
        Fixture f;
        require(!f.locks.enter(section, owner_thread), "abandonment fixture owner acquires");
        auto abandoned = f.locks.enter(section, main_thread);
        auto peer = f.locks.enter(section, other_thread);
        require(abandoned && peer, "abandonment fixture admits both tokens");
        if (selected) f.locks.leave(section, owner_thread);
        const auto before = f.bytes();
        auto result = std::async(std::launch::async, [&] { return peer->wait({}); });
        StopLocks cleanup{f.locks};
        abandoned.reset();
        require(!ready(result, "abandoned admission must wake the other waiter as cancelled"),
                "dropping an uncompleted token terminates rather than silently stranding admitted peers");
        rejects([&] { (void)f.locks.enter(second_section, late_thread); });
        rejects([&] { f.locks.complete(*peer); });
        require(f.bytes() == before, "abandonment cancels host state without ungated guest count changes");
    }
}

void real_execution_handoff_allows_original_owner_to_release() {
    Fixture f;
    sfr::GuestExecution execution;
    auto owner = execution.enter(3);
    require(!f.locks.enter(section, owner_thread), "worker thread owns section before scheduling handoff");
    std::promise<void> admitted;
    auto admitted_future = admitted.get_future();
    auto contender = std::async(std::launch::async, [&] {
        auto lease = execution.enter(1);
        auto pending = f.locks.enter(section, main_thread);
        require(bool(pending), "scheduled main contender cannot acquire worker-owned section");
        admitted.set_value();
        lease->run_blocking([&](std::stop_token stop) {
            if (!pending->wait(stop)) throw sfr::GuestExecutionCancelled();
        });
        lease->checkpoint(false);
        f.locks.complete(*pending);
        f.state(0, 1, main_thread);
        f.locks.leave(section, main_thread);
    });
    StopExecution cleanup{execution, f.locks};
    execution.wait_until_ready(1);
    owner->checkpoint();
    ready(admitted_future, "main waiter registers before releasing its execution permit");
    f.state(1, 1, owner_thread);
    f.locks.leave(section, owner_thread);
    owner.reset();
    ready(contender, "contender resumes only after original owner can run and release");
    auto verifier = execution.enter(4);
    f.state(0xFFFFFFFF, 0, 0);
}

void real_execution_stop_drains_all_blocked_contenders_without_acquisition() {
    Fixture f;
    sfr::GuestExecution execution;
    auto owner = execution.enter(3);
    require(!f.locks.enter(section, owner_thread), "stop fixture owner acquires");
    std::atomic<unsigned> acquired = 0;
    auto contend = [&](uint32_t id, uint32_t thread) {
        try {
            auto lease = execution.enter(id);
            auto pending = f.locks.enter(section, thread);
            require(bool(pending), "stop fixture contender is pending");
            lease->run_blocking([&](std::stop_token stop) {
                if (!pending->wait(stop)) throw sfr::GuestExecutionCancelled();
            });
            f.locks.complete(*pending);
            ++acquired;
            return false;
        } catch (const sfr::GuestExecutionCancelled&) { return true; }
    };
    auto one = std::async(std::launch::async, [&] { return contend(1, main_thread); });
    auto two = std::async(std::launch::async, [&] { return contend(2, other_thread); });
    StopExecution cleanup{execution, f.locks};
    execution.wait_until_ready(1);
    execution.wait_until_ready(2);
    owner->checkpoint();
    f.state(2, 1, owner_thread);
    const auto before = f.bytes();
    owner.reset();
    execution.stop_and_drain();
    require(ready(one, "global execution stop wakes first critical contender") &&
                ready(two, "global execution stop wakes second critical contender") && acquired == 0,
            "stop/drain cancels actual blocked threads without reporting acquisition");
    require(f.bytes() == before, "terminal stop does not fabricate ownership or decrement guest counts ungated");
}
}

int main() {
    unsigned failures = 0;
    for (auto test : {observed_contention_has_registration_before_park_and_exact_handoff,
                      recursion_multiple_waiters_and_handoff_prevent_barging,
                      independent_addresses_and_foreign_tokens_cannot_cross_handoff,
                      invalid_states_and_late_guards_have_no_partial_effects,
                      live_reservation_and_invalid_leave_do_not_admit_phantom_waiters,
                      signed_lock_count_boundaries_reject_before_admission_or_recursion,
                      every_enter_allocation_failure_preserves_bytes_and_coordinator_usability,
                      write_combined_memory_retains_protection_and_full_state,
                      terminal_cancellation_wakes_waiters_and_never_acquires,
                      pending_token_survives_coordinator_destruction_as_cancelled,
                      initialization_preserves_existing_layout_and_rejects_active_lifetimes,
                      abandoning_uncompleted_admission_stops_peers_without_guest_mutation,
                      real_execution_handoff_allows_original_owner_to_release,
                      real_execution_stop_drains_all_blocked_contenders_without_acquisition}) {
        try { test(); }
        catch (const std::exception& error) { std::cerr << error.what() << '\n'; ++failures; }
    }
    return failures ? 1 : 0;
}
