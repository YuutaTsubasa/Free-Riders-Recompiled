#include "critical_section.h"

namespace sfr {
namespace {
constexpr uint32_t maximum_recursion = 0x7fffffffu;

struct CriticalSectionState {
    uint32_t lock;
    uint32_t recursion;
    uint32_t owner;
};

CriticalSectionState checked_state(GuestMemory& memory, uint32_t address, uint32_t thread) {
    if ((address & 3u) != 0)
        throw RuntimeStop("critical-section-invalid", address, "critical section must be 4-byte aligned");
    if (thread == 0)
        throw RuntimeStop("critical-section-invalid", address, "current guest thread object is null");

    const uint64_t base = address;
    // Validate even opaque header/list bytes and the tail before any mutation.
    memory.check_write(base, 28);
    if (memory.load<uint8_t>(base) != 1 || memory.load<uint32_t>(base + 4) != 0)
        throw RuntimeStop("critical-section-invalid", address, "expected an unsignaled critical-section event");

    CriticalSectionState state{memory.load<uint32_t>(base + 16),
                               memory.load<uint32_t>(base + 20),
                               memory.load<uint32_t>(base + 24)};
    const bool free = state.lock == 0xffffffffu && state.recursion == 0 && state.owner == 0;
    const bool held = state.recursion >= 1 && state.recursion <= maximum_recursion &&
                      state.owner != 0 && state.lock == state.recursion - 1;
    if (!free && !held)
        throw RuntimeStop("critical-section-invalid", address, "inconsistent lock state or unsupported waiter count");
    return state;
}

void store_state(GuestMemory& memory, uint32_t address, const CriticalSectionState& state) {
    const uint64_t base = address;
    // Deliberate big-endian guest representation for this single-thread runner.
    // Xenia uses native host atomic lock_count access; no such host overlay,
    // mutex, spin loop, or waiter/wakeup behavior is supported here.
    memory.store<uint32_t>(base + 16, state.lock);
    memory.store<uint32_t>(base + 20, state.recursion);
    memory.store<uint32_t>(base + 24, state.owner);
}
}

void enter_critical_section(GuestMemory& memory, uint32_t address, uint32_t thread) {
    auto state = checked_state(memory, address, thread);
    if (state.recursion == 0) {
        state = {0, 1, thread};
    } else {
        if (state.owner != thread)
            throw RuntimeStop("critical-section-contention", address, "critical section belongs to another guest thread");
        if (state.recursion == maximum_recursion)
            throw RuntimeStop("critical-section-invalid", address, "critical-section recursion would overflow");
        ++state.lock;
        ++state.recursion;
    }
    store_state(memory, address, state);
}

void leave_critical_section(GuestMemory& memory, uint32_t address, uint32_t thread) {
    auto state = checked_state(memory, address, thread);
    if (state.recursion == 0 || state.owner != thread)
        throw RuntimeStop("critical-section-invalid", address, "current guest thread does not own critical section");
    if (state.recursion == 1) {
        state = {0xffffffffu, 0, 0};
    } else {
        --state.lock;
        --state.recursion;
    }
    store_state(memory, address, state);
}

void initialize_critical_section(GuestMemory& memory, uint32_t address) {
    // Xenia X_RTL_CRITICAL_SECTION layout, revision 95a5c3ee250f80c3b9d139658649d9ffb6db3eec.
    constexpr uint64_t critical_section_size = 28;
    const uint64_t base = address;
    memory.check_write(base, critical_section_size);

    memory.store<uint8_t>(base, 1);
    memory.store<uint8_t>(base + 1, 0);
    memory.store<uint32_t>(base + 4, 0);
    memory.store<uint32_t>(base + 16, 0xffffffffu);
    memory.store<uint32_t>(base + 20, 0);
    memory.store<uint32_t>(base + 24, 0);
}

uint32_t initialize_critical_section_and_spin_count(GuestMemory& memory, uint32_t address,
                                                    uint32_t spin_count) {
    if (spin_count > 0xffffff00u)
        throw RuntimeStop("critical-section-spin", address,
                          "spin count cannot be converted without unsigned overflow");

    uint32_t units = (spin_count + 255u) >> 8;
    if (units > 255u) units = 255u;
    initialize_critical_section(memory, address);
    memory.store<uint8_t>(uint64_t(address) + 1, static_cast<uint8_t>(units));
    return 0;
}
}
