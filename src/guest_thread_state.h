#pragma once
#include <cstdint>

namespace sfr {
// A lwarx/ldarx reservation belongs to the host thread that made it, as each
// console core has its own: guest threads running at once must not see
// each other's. Active when owner is the id of the GuestMemory it was made
// in (never 0; ids are not reused, unlike addresses of destroyed instances).
// version is the reservation stripe's count of successful conditional stores
// when the reservation was made (see guest_memory.cpp).
struct GuestReservation {
    uint64_t owner = 0;
    uint64_t address = 0, value = 0, version = 0;
    uint8_t size = 0;
};

struct GuestEntryState {
    // The permit needs one checkpoint in every few dozen (guest_checkpoint).
    uint32_t checkpoint_countdown = 0;
    // The function this thread entered last (named when it stops).
    const char* current_function = "";
    uint32_t current_address = 0;
    // Whether this thread's entries do more than checkpoint and name
    // themselves: a guest running beside the permit, an audit in progress,
    // the entry diagnostics. Until a thread sets it from what applies, every
    // entry takes the full path.
    bool observed = true;
    // Whether anything beyond the permit wants to see every entry: the entry
    // diagnostics, a reach log, an audit. A guest playing beside the permit
    // is observed without being watched, and leaves early -- which keeps the
    // thread_locals those three read out of every entry of a race.
    bool watched = true;
    // A guest running beside the permit rather than holding it, and how it
    // follows a hooked call (diagnostic_main.cpp's parallel_function_entry).
    bool parallel = false;
    bool detach_at_entry = false;
    uint32_t hook_stack_pointer = 0;
};

// A function entry and its scalar stores repeatedly consult both records.
// One TLS object lets ARM64 keep a single resolved address across those
// accesses. Separate TLS variables made a four-register save resolve TLS
// six times on Android. Each host thread still owns both records independently.
struct GuestThreadState {
    GuestEntryState entry;
    GuestReservation reservation;
};
inline thread_local GuestThreadState guest_thread_state;
}
