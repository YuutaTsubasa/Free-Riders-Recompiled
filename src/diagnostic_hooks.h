#pragma once
#include <atomic>
#include <optional>
#include "guest_memory.h"
#include "integer_arithmetic.h"
#include "store_halfword_update.h"
#include "store_float_single_update.h"
#include "load_halfword_update.h"
#include "memory_update_forms.h"
#include "vector_integer.h"
#include "counted_branch.h"
struct PPCContext;
namespace sfr {
extern GuestMemory* active_memory;
[[noreturn]] void unsupported_function(PPCContext&, const char*, uint32_t, const char*);
void dispatch_import(PPCContext&, const char*, uint32_t);
// Our replacements of original functions (SFR_HOOK) reach host state, so a
// guest running beside the main thread must hold the permit inside them.
bool register_hook(const char* name);  // "sub_XXXXXXXX"
// The hooked guest functions, a bit each over the guest image. is_hook()
// answers at every entry of a guest running beside the permit, and the hash
// lookup it used to be was 3.7% of a race on the phone (and a call through
// the PLT besides). Half a megabyte of .bss answers it in a load and a test.
// Anything outside the image -- which no hook has ever been, every one of
// them naming a function of the title -- still goes to the registry.
constexpr uint32_t hook_base = 0x82000000, hook_limit = 0x83000000;
inline uint64_t hook_bits[(hook_limit - hook_base) / 4 / 64];
bool is_hook_outside_the_image(uint32_t address);
inline bool is_hook(uint32_t address) {
    const uint32_t offset = address - hook_base;
    if (offset >= hook_limit - hook_base) [[unlikely]] return is_hook_outside_the_image(address);
    if (address & 3) return false;  // a function begins on a word
    const uint32_t index = offset / 4;
    return (hook_bits[index / 64] >> (index % 64)) & 1;
}
// Whether a guest function entry does more than name itself and checkpoint.
// Everything else it does is observation: the ORIGINAL_* audits, the entry
// traces and dumps, the sampler's address for SFR_SAMPLE_PROFILE. Playing turns it
// off with SFR_DIAGNOSTIC_ENTRIES=0, which costs the run its LAST_FUNCTION
// detail and those audits; SFR_SAMPLE_PROFILE and SFR_HOST_PROFILE turn it back on
// because they read what it records.
extern const bool diagnostic_entries;
// Generated code checkpoints at every function entry and loop, millions of
// times a second; the permit needs one of every few dozen to hand off on
// time and to notice cancellation, so the rest return here, inline.
void guest_checkpoint_permit();

// What a guest function entry reads and writes, in one object.
//
// These were separate thread_local variables, which is the same thing on a
// desktop and not on Android: there a thread_local lives in a dynamically
// loaded module's TLS block, and each *variable* costs a call to the
// linker's tlsdesc resolver. An entry touched seven of them, several million
// times a second, and that resolver was 9.5% of a race on the phone. One
// object is one resolution; the fields are offsets from it.
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
inline thread_local GuestEntryState guest_entry;

inline void guest_checkpoint() {
    if (guest_entry.checkpoint_countdown) [[likely]] {
        --guest_entry.checkpoint_countdown;
        return;
    }
    guest_checkpoint_permit();
}
void enter_function_observed(PPCContext&, const char*, uint32_t);
// Every guest function entry; inline, as it runs millions of times a second.
inline void enter_function(PPCContext& ctx, const char* name, uint32_t address) {
    GuestEntryState& entry = guest_entry;
    if (entry.observed) [[unlikely]] {
        enter_function_observed(ctx, name, address);
        return;
    }
    if (entry.checkpoint_countdown) [[likely]] --entry.checkpoint_countdown;
    else guest_checkpoint_permit();
    entry.current_function = name;
    entry.current_address = address;
}
void call_indirect(PPCContext&, uint8_t*, uint32_t);
uint64_t read_time_base();
uint32_t load_reserved_word(PPCContext&, uint64_t);
void store_conditional_word(PPCContext&, uint64_t, uint32_t);
uint64_t load_reserved_doubleword(PPCContext&, uint64_t);
void store_conditional_doubleword(PPCContext&, uint64_t, uint64_t);
void load_vector_memory(uint32_t, uint8_t (&)[16]);
void load_vector_left(uint32_t, uint8_t (&)[16]);
void load_vector_right(uint32_t, uint8_t (&)[16]);
void store_vector_memory(uint32_t, const uint8_t (&)[16]);
void store_vector_word(uint32_t, const uint8_t (&)[16]);
void store_vector_left(uint32_t, const uint8_t (&)[16]);
void store_vector_right(uint32_t, const uint8_t (&)[16]);
void zero_cache_block(uint32_t);
void zero_cache_line(uint32_t);
void synchronize_resource_memory(PPCContext&);
// Frames presented so far (guest_graphics_hooks.cpp).
extern std::atomic<uint32_t> present_count;
// SFR_WATCH_WORD=<hex address>: while this holds an address, every guest
// function entry reports a change of that word together with the function
// entered, which names the code that wrote it. A hook may set it for an
// address only known at run time. Zero is off.
extern std::atomic<uint32_t> watch_word;
// Nanoseconds the main thread spent waiting for the GPU (within main_blocked).
extern std::atomic<uint64_t> main_gpu_wait_ns;
// Runs a host wait for the current guest thread without its execution
// permit, so the other guests run meanwhile.
void wait_without_permit(void (*wait)(void*), void* argument);
// Whether the per-call graphics traces (texture bindings, render and sampler
// state, blend requests, viewports) are printed. They are the audit trail of
// each hooked entry, and about five thousand lines a frame, so playing turns
// them off with SFR_TRACE_GRAPHICS=0.
bool graphics_trace();
struct GamepadState;
// Kinect emulation (nui_hooks.cpp): user 0's pad drives the skeleton, and a
// host thread signals the title's next-frame event at 30 Hz.
GamepadState nui_gamepad();
// The pad standing beside the first player, when one is connected: an empty
// result means the title sees one Kinect player, as it did before. Which pad
// that is depends on who the first player is -- the second pad normally, but
// the first pad once a camera has taken the first player's body over.
std::optional<GamepadState> nui_second_gamepad(uint32_t user, bool racing);
void start_nui_skeleton_events(uint32_t event_handle);
void stop_nui_skeleton_events();
}
// Scalar accesses are bounded and big-endian. Recognized reservation pairs use
// explicit hooks; other raw guest-memory bodies remain diagnostic stops.
#define PPC_LOAD_U8(x) sfr::active_memory->load<uint8_t>(uint64_t(x))
#define PPC_LOAD_U16(x) sfr::active_memory->load<uint16_t>(uint64_t(x))
#define PPC_LOAD_U32(x) sfr::active_memory->load<uint32_t>(uint64_t(x))
#define PPC_LOAD_U64(x) sfr::active_memory->load<uint64_t>(uint64_t(x))
#define PPC_STORE_U8(x,y) sfr::active_memory->store<uint8_t>(uint64_t(x), uint8_t(y))
#define PPC_STORE_U16(x,y) sfr::active_memory->store<uint16_t>(uint64_t(x), uint16_t(y))
#define PPC_STORE_U32(x,y) sfr::active_memory->store<uint32_t>(uint64_t(x), uint32_t(y))
#define PPC_STORE_U64(x,y) sfr::active_memory->store<uint64_t>(uint64_t(x), uint64_t(y))
#define PPC_CALL_INDIRECT_FUNC(x) sfr::call_indirect(ctx, base, uint32_t(x))

// Defines a replacement of original function x and records it as a hook.
#define SFR_HOOK(x) [[maybe_unused]] static const bool x##_hook = ::sfr::register_hook(#x); PPC_FUNC(x)
// A replacement that touches only guest memory, its arguments and atomics,
// so a detached guest runs it without taking the execution permit. Hot
// functions only: a hook called by every thread brought them all back to
// the permit (docs/performance.md).
#define SFR_CONCURRENT_HOOK(x) PPC_FUNC(x)
