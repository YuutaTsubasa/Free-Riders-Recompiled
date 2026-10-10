#pragma once
#include <atomic>
#include <optional>
#if defined(__SSSE3__)
#include <tmmintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#include "guest_memory.h"
#include "integer_arithmetic.h"
#include "store_halfword_update.h"
#include "store_float_single_update.h"
#include "vector_compare_bounds.h"
#include "vector_unpack.h"
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
// The same bits folded into 4 KiB, which stays in the L1 cache: the main
// thread runs beside the permit and asks at nearly every entry, and the
// functions of a race frame touch more of the half megabyte than an AYN
// Thor's L2 holds. A clear bit answers no; a set one asks hook_bits.
inline constexpr uint32_t hook_filter_bits = 32768;
inline uint64_t hook_filter[hook_filter_bits / 64];
bool is_hook_outside_the_image(uint32_t address);
inline bool is_hook(uint32_t address) {
    const uint32_t offset = address - hook_base;
    if (offset >= hook_limit - hook_base) [[unlikely]] return is_hook_outside_the_image(address);
    if (address & 3) return false;  // a function begins on a word
    const uint32_t index = offset / 4;
    const uint32_t folded = index % hook_filter_bits;
    if (!((hook_filter[folded / 64] >> (folded % 64)) & 1)) [[likely]] return false;
    return (hook_bits[index / 64] >> (index % 64)) & 1;
}
// Hooks of the title's Direct3D (guest_graphics_hooks.cpp): with
// SFR_PARALLEL_MAIN they take the graphics lock, which keeps one thread at a
// time in the native device, and those that reach no other host state
// (graphics_only) not the global permit. Unleashed and Marathon Recompiled
// guard their device the same way, with no lock around the whole program.
inline uint64_t graphics_hook_bits[(hook_limit - hook_base) / 4 / 64];
inline uint64_t graphics_only_hook_bits[(hook_limit - hook_base) / 4 / 64];
bool register_graphics_hook(const char* name, bool host_state);
// Hooks of the title's motion input -- its skeleton frames and the race's
// readers of them (nui_hooks.cpp, nui_race_hooks.cpp) -- take the input lock
// instead of the global permit: what they share is the emulated players'
// state, not the kernel's.
inline uint64_t input_hook_bits[(hook_limit - hook_base) / 4 / 64];
bool register_input_hook(const char* name);
// Hooks of the title's menus -- the pad and voice standing in for the hand
// cursor (nui_hooks.cpp) -- take the menu lock: what they share is the menu
// emulation's state (the buttons pressed since the last input update, the
// manager last updated, the scripted words).
inline uint64_t menu_hook_bits[(hook_limit - hook_base) / 4 / 64];
bool register_menu_hook(const char* name);
// The graphics lock around host code outside the graphics hooks that changes
// the native device (the Avatar's model drawn from its draw hook). Taken
// after the input and menu locks, never before them.
struct HostGraphicsScope {
    HostGraphicsScope();
    ~HostGraphicsScope();
    HostGraphicsScope(const HostGraphicsScope&) = delete;
    HostGraphicsScope& operator=(const HostGraphicsScope&) = delete;
private:
    bool owned_ = false;
};
inline bool test_hook_bit(const uint64_t* bits, uint32_t address) {
    const uint32_t offset = address - hook_base;
    if (offset >= hook_limit - hook_base || (address & 3)) return false;
    const uint32_t index = offset / 4;
    return (bits[index / 64] >> (index % 64)) & 1;
}
// Whether a guest function entry does more than name itself and checkpoint.
// Everything else it does is observation: the ORIGINAL_* audits, the entry
// traces and dumps, the sampler's address for SFR_SAMPLE_PROFILE. Playing turns it
// off with SFR_DIAGNOSTIC_ENTRIES=0, which costs the run its LAST_FUNCTION
// detail and those audits; SFR_SAMPLE_PROFILE and SFR_HOST_PROFILE turn it back on
// because they read what it records.
extern const bool diagnostic_entries;
// Generated code checkpoints at every function entry and loop, millions of
// times a second; the permit needs only a fraction of those entries to hand off on
// time and to notice cancellation, so the rest return here, inline.
void guest_checkpoint_permit();



inline void guest_checkpoint() {
    if (guest_thread_state.entry.checkpoint_countdown) [[likely]] {
        --guest_thread_state.entry.checkpoint_countdown;
        return;
    }
    guest_checkpoint_permit();
}
void enter_function_observed(PPCContext&, const char*, uint32_t);
// Gives back the permit and graphics lock the outermost hook's entry took.
void hook_scope_exit() noexcept;
struct HookScope {
    HookScope() { ++guest_thread_state.entry.hook_depth; }
    ~HookScope() {
        auto& entry = guest_thread_state.entry;
        if (!--entry.hook_depth && (entry.hook_stack_pointer || entry.graphics_stack_pointer || entry.input_stack_pointer))
            hook_scope_exit();
    }
    HookScope(const HookScope&) = delete;
    HookScope& operator=(const HookScope&) = delete;
};
// The calling thread's state, kept in the context's reservation register,
// which the generated code never uses (sfr::load_reserved_word keeps the
// reservation in GuestThreadState). On Android libmain is loaded after start,
// so its thread_locals are dynamic TLS: every guest function entry called
// the linker's TLS resolver, 5% of the main thread of an AYN Thor race.
// Every context belongs to one host thread (a new guest thread gets a new
// one), and the first entry on a context binds it.
GuestThreadState& bind_guest_thread_state(uint64_t& slot) noexcept;
template <class Context>
inline GuestThreadState& entry_thread_state(Context& ctx) {
    if constexpr (requires { ctx.reserved.u64; }) {
        if (ctx.reserved.u64) [[likely]] return *reinterpret_cast<GuestThreadState*>(ctx.reserved.u64);
        return bind_guest_thread_state(ctx.reserved.u64);
    } else {
        return guest_thread_state;
    }
}
// The checkpoint at every loop label of the generated code, with the state
// its context holds: through TLS, each label not dominated by another paid
// the resolver again.
template <class Context>
inline void guest_checkpoint(Context& ctx) {
    GuestEntryState& entry = entry_thread_state(ctx).entry;
    if (entry.checkpoint_countdown) [[likely]] {
        --entry.checkpoint_countdown;
        return;
    }
    guest_checkpoint_permit();
}
// Every guest function entry; inline, as it runs millions of times a second.
// A template so that the body sees the generated PPCContext, which is
// defined after this header.
template <class Context>
inline void enter_function(Context& ctx, const char* name, uint32_t address) {
    GuestEntryState& entry = entry_thread_state(ctx).entry;
    // A guest running beside the permit is observed only for hooks: an entry
    // that is no hook, outside every hook, needs nothing more than the fast
    // path. Taken out of line every time, these entries cost the main thread
    // about 4 ms of an AYN Thor race frame once it ran beside the permit.
    if (entry.observed) [[unlikely]] {
        if (!entry.parallel || entry.watched || entry.hook_stack_pointer || entry.graphics_stack_pointer ||
            entry.input_stack_pointer ||
            entry.detach_at_entry || is_hook(address)) {
            enter_function_observed(ctx, name, address);
            return;
        }
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
// The sixteen bytes of an aligned vector reversed (the register holds byte 15
// of memory in element 0), in one shuffle where SSSE3 or NEON exists.
inline void reverse_vector(const uint8_t* from, uint8_t* to) {
#if defined(__SSSE3__)
    const __m128i order = _mm_setr_epi8(15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(to),
                     _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(from)), order));
#elif defined(__ARM_NEON)
    // Reverse the bytes of each half, then exchange the halves.
    const uint8x16_t halves = vrev64q_u8(vld1q_u8(from));
    vst1q_u8(to, vextq_u8(halves, halves, 8));
#else
    for (unsigned i = 0; i < 16; ++i) to[i] = from[15 - i];
#endif
}
// Vector loads and stores: an aligned vector in one ordinary page is read or
// written here, inline in the generated code; everything else (special and
// watched pages, a reservation, the first ones logged) goes to the checked
// path. As a call every time they were 3.8% of a Thor race's main thread and
// 8.1% of its busiest worker.
void load_vector_memory_slow(uint32_t, uint8_t (&)[16]);
void store_vector_memory_slow(uint32_t, const uint8_t (&)[16]);
// The first few of each still go the checked way, which logs them.
inline uint32_t vector_load_traces = 4, vector_store_traces = 4;
// GuestMemory::base() of active_memory, for the direct accesses below.
inline uint8_t* guest_base = nullptr;
#if SFR_DIRECT_MEMORY
// SFR_DIRECT_MEMORY: guest memory is read and written as base + address, as
// in Unleashed and Marathon Recompiled (their ppc_context.h); nothing is
// checked, logged or watched per access (see GuestMemory::direct_guest_access).
inline void load_vector_memory(uint32_t address, uint8_t (&destination)[16]) {
    reverse_vector(guest_base + (address & ~0xFu), destination);
}
inline void store_vector_memory(uint32_t address, const uint8_t (&source)[16]) {
    reverse_vector(source, guest_base + (address & ~0xFu));
}
void load_vector_left(uint32_t, uint8_t (&)[16]);
void load_vector_right(uint32_t, uint8_t (&)[16]);
inline void zero_cache_block(uint32_t address) { std::memset(guest_base + (address & ~31u), 0, 32); }
inline void zero_cache_line(uint32_t address) { std::memset(guest_base + (address & ~127u), 0, 128); }
#else
inline void load_vector_memory(uint32_t address, uint8_t (&destination)[16]) {
    if (active_memory && !vector_load_traces) [[likely]]
        if (const uint8_t* bytes = active_memory->fast_read(address & ~0xFu, 16)) [[likely]] {
            reverse_vector(bytes, destination);
            return;
        }
    load_vector_memory_slow(address, destination);
}
void load_vector_left(uint32_t, uint8_t (&)[16]);
void load_vector_right(uint32_t, uint8_t (&)[16]);
inline void store_vector_memory(uint32_t address, const uint8_t (&source)[16]) {
    if (active_memory && !vector_store_traces) [[likely]]
        if (uint8_t* bytes = active_memory->fast_write(address & ~0xFu, 16)) [[likely]] {
            reverse_vector(source, bytes);
            return;
        }
    store_vector_memory_slow(address, source);
}
void zero_cache_block(uint32_t);
void zero_cache_line(uint32_t);
#endif
void store_vector_word(uint32_t, const uint8_t (&)[16]);
void store_vector_left(uint32_t, const uint8_t (&)[16]);
void store_vector_right(uint32_t, const uint8_t (&)[16]);
void synchronize_resource_memory(PPCContext&);
// Frames presented so far (guest_graphics_hooks.cpp).
extern std::atomic<uint32_t> present_count;
// The present at which the last SFR_SAY word was said (0 until then).
extern std::atomic<uint32_t> say_done_present;
// Runtime guest identity (the main guest is 1), independent of the host TID.
uint32_t current_guest_thread_id();
// SFR_WATCH_WORD=<hex address>: while this holds an address, every guest
// function entry reports a change of that word together with the function
// entered, which names the code that wrote it. A hook may set it for an
// address only known at run time. Zero is off.
extern std::atomic<uint32_t> watch_word;
// Nonzero while the race job dispatcher 8249F048 runs (game_patches.cpp).
extern std::atomic<uint32_t> race_jobs_dispatching;
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
// Controller `user` (0 is the first player's, keyboard included), or none.
std::optional<GamepadState> nui_pad(uint32_t user);
// Relay Race: which controller drives the racer who is up (0..3).
void set_relay_pad(uint32_t user);
uint32_t relay_pad();
// Source selected for logical P1's latest submitted skeleton. P1's pad
// remains available while Camera is enabled; race consumers follow this.
bool camera_motion_active();
// Advances only when a newly inferred Camera pose is submitted to the guest.
uint64_t camera_pose_generation();
// Host monotonic time; title clock fields are not seconds.
uint64_t camera_motion_clock_ns();
// The pad standing beside the first player, when one is connected: an empty
// result means the title sees one Kinect player. Camera does not change
// controller assignment: the configured P2 controller remains user 1.
std::optional<GamepadState> nui_second_gamepad(uint32_t user, bool racing);
// Whether a real Kinect (SFR_CAMERA=kinect) tracks the players' bodies. The
// race then reads them through the title's own body record and gesture
// detectors, and the "On your Gear!" measurements run as on the console,
// instead of the pad or the camera's motion standing in for them
// (nui_race_hooks.cpp).
bool nui_body_from_sensor();
// True only if the selected backend successfully opened its depth stream.
bool nui_sensor_has_depth();
// Advances with each new frame or expired tracking update (0 without one): the race
// steers from its bodies only when a frame is new (nui_race_hooks.cpp).
uint64_t kinect_frame_generation();
// The second player's pad as the Kinect emulation last read it, for the race
// hooks. Empty when there is no second player.
std::optional<GamepadState> second_player_pad();
void publish_second_player_pad(const std::optional<GamepadState>& pad);
void start_nui_skeleton_events(uint32_t event_handle);
void stop_nui_skeleton_events();
}
// Scalar accesses are bounded and big-endian. Recognized reservation pairs use
// explicit hooks; other raw guest-memory bodies remain diagnostic stops.
#if SFR_DIRECT_MEMORY
// Accesses go straight to base, so a function has nothing to take at entry.
#define SFR_FAST_PATH() ((void)0)
// As XenonRecomp's own ppc_context.h: volatile, so a thread's accesses are
// neither merged nor reordered by the compiler (guests run in parallel).
#define PPC_LOAD_U8(x) (*(volatile uint8_t*)(base + uint32_t(x)))
#define PPC_LOAD_U16(x) __builtin_bswap16(*(volatile uint16_t*)(base + uint32_t(x)))
#define PPC_LOAD_U32(x) __builtin_bswap32(*(volatile uint32_t*)(base + uint32_t(x)))
#define PPC_LOAD_U64(x) __builtin_bswap64(*(volatile uint64_t*)(base + uint32_t(x)))
#define PPC_STORE_U8(x,y) (*(volatile uint8_t*)(base + uint32_t(x)) = uint8_t(y))
#define PPC_STORE_U16(x,y) (*(volatile uint16_t*)(base + uint32_t(x)) = __builtin_bswap16(uint16_t(y)))
#define PPC_STORE_U32(x,y) (*(volatile uint32_t*)(base + uint32_t(x)) = __builtin_bswap32(uint32_t(y)))
#define PPC_STORE_U64(x,y) (*(volatile uint64_t*)(base + uint32_t(x)) = __builtin_bswap64(uint64_t(y)))
#else
//
// A function that begins with SFR_FAST_PATH() (scripts/generate_diagnostic.py
// puts it after each PPC_FUNC_PROLOGUE) reads what every access needs from
// the GuestMemory once (GuestMemory::FastPath) and keeps it, with its base
// argument, in locals: without strict aliasing, each guest store otherwise
// makes the compiler load active_memory, the page table and the base again.
// Elsewhere sfr_fast names the namespace-scope NoFastPath below, and the
// accesses are the GuestMemory member calls they always were.
namespace sfr {
struct NoFastPath {};
template<typename T> inline T guest_load(NoFastPath, const uint8_t*, uint64_t address) {
    return active_memory->load<T>(address);
}
template<typename T> inline void guest_store(NoFastPath, uint8_t*, uint64_t address, T value) {
    active_memory->store<T>(address, value);
}
template<typename T> __attribute__((always_inline)) inline T guest_load(const GuestMemory::FastPath& fast,
                                                                        const uint8_t* base, uint64_t address) {
    return GuestMemory::load<T>(fast, base, address);
}
template<typename T> __attribute__((always_inline)) inline void guest_store(const GuestMemory::FastPath& fast,
                                                                            uint8_t* base, uint64_t address, T value) {
    GuestMemory::store<T>(fast, base, address, value);
}
}
inline constexpr sfr::NoFastPath sfr_fast{};
#define SFR_FAST_PATH() const ::sfr::GuestMemory::FastPath sfr_fast = ::sfr::active_memory->fast_path()
#define PPC_LOAD_U8(x) sfr::guest_load<uint8_t>(sfr_fast, base, uint64_t(x))
#define PPC_LOAD_U16(x) sfr::guest_load<uint16_t>(sfr_fast, base, uint64_t(x))
#define PPC_LOAD_U32(x) sfr::guest_load<uint32_t>(sfr_fast, base, uint64_t(x))
#define PPC_LOAD_U64(x) sfr::guest_load<uint64_t>(sfr_fast, base, uint64_t(x))
#define PPC_STORE_U8(x,y) sfr::guest_store<uint8_t>(sfr_fast, base, uint64_t(x), uint8_t(y))
#define PPC_STORE_U16(x,y) sfr::guest_store<uint16_t>(sfr_fast, base, uint64_t(x), uint16_t(y))
#define PPC_STORE_U32(x,y) sfr::guest_store<uint32_t>(sfr_fast, base, uint64_t(x), uint32_t(y))
#define PPC_STORE_U64(x,y) sfr::guest_store<uint64_t>(sfr_fast, base, uint64_t(x), uint64_t(y))
#endif
#define PPC_CALL_INDIRECT_FUNC(x) sfr::call_indirect(ctx, base, uint32_t(x))

// Defines a replacement of original function x and records it as a hook.
// The replacement runs inside a HookScope, so the permit and graphics lock a
// parallel guest took at the hook's entry are given back as it returns.
// (Inferring the return from the stack pointer of a later entry missed a
// thread that calls the hook from a loop in the same frame: the Kinect
// skeleton thread then held the global permit for good after its first
// NuiSkeletonGetNextFrame, stalling the whole game for its time slices.)
#define SFR_SCOPED_HOOK(x, registration) [[maybe_unused]] static const bool x##_hook = registration; \
    static void x##_hooked(PPCContext& __restrict ctx, uint8_t* base); \
    PPC_FUNC(x) { ::sfr::HookScope scope; x##_hooked(ctx, base); } \
    static void x##_hooked(PPCContext& __restrict ctx, uint8_t* base)
#define SFR_HOOK(x) SFR_SCOPED_HOOK(x, ::sfr::register_hook(#x))
// A Direct3D replacement that touches only the native device and guest memory
// (see graphics_hook_bits), and one that also reaches other host state.
#define SFR_GRAPHICS_HOOK(x) SFR_SCOPED_HOOK(x, ::sfr::register_graphics_hook(#x, false))
#define SFR_GRAPHICS_HOST_HOOK(x) SFR_SCOPED_HOOK(x, ::sfr::register_graphics_hook(#x, true))
#define SFR_INPUT_HOOK(x) SFR_SCOPED_HOOK(x, ::sfr::register_input_hook(#x))
#define SFR_MENU_HOOK(x) SFR_SCOPED_HOOK(x, ::sfr::register_menu_hook(#x))
// A replacement that touches only guest memory, its arguments, atomics and
// host objects that keep their own lock, so a detached guest runs it without
// taking any lock of the runtime's (docs/architecture-migration.md, phase 4).
#define SFR_CONCURRENT_HOOK(x) PPC_FUNC(x)
