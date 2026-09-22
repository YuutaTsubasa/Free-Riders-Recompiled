#include "diagnostic_hooks.h"
#include "ppc_recomp_shared.h"
#include "xex_module.h"
#include "virtual_memory.h"
#include "critical_section.h"
#include "hardware_info.h"
#include "thread_local_storage.h"
#include "system_time.h"
#include "guest_clock.h"
#include "timestamp_bundle.h"
#include "vector_memory.h"
#include "optional_import_policy.h"
#include "native_modules.h"
#include "native_language.h"
#include "native_country.h"
#include "system_config.h"
#include "game_region.h"
#include "memory_statistics.h"
#include "video_mode.h"
#include "ansi_string.h"
#include "guest_execution.h"
#include "guest_critical_sections.h"
#include "debug_monitor.h"
#include "nui_device_status.h"
#include "unselected_users.h"
#include "local_profile.h"
#include "content_files.h"
#include "multibyte_unicode.h"
#include "render_state_entries.h"
#include "image_protection.h"
#include "resource_coherency.h"
#include "video_globals.h"
#include "native_winsock.h"
#include "native_notifications.h"
#include "notification_placement.h"
#include "native_graphics.h"
#include "guest_graphics.h"
#include "native_formats.h"
#include "native_renderer.h"
#include "runtime_shader_cache.h"
#if defined(__SSSE3__)
#include <immintrin.h>
#endif
#include "native_shaders.h"
#include "asset_files.h"
#include "guest_files.h"
#include "guest_async_files.h"
#include "native_sync_objects.h"
#include "guest_sync_objects.h"
#include "guest_wait.h"
#include "guest_threads.h"
#include "native_input.h"
#include "native_audio.h"
#include "guest_printf.h"
#include "native_presentation.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <TinySHA1.hpp>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

static std::string fingerprint(const void* data, size_t size) {
    sha1::SHA1 hash;
    uint8_t digest[20];
    hash.processBytes(data, size);
    hash.finalize(digest);
    const char* hex = "0123456789abcdef";
    std::string result;
    for (auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}

namespace sfr {
GuestMemory* active_memory = nullptr;
static std::unordered_map<uint32_t, PPCFunc*> functions;
// Names come from string literals (generated code and hooks): keep the pointer,
// as copying the name at every guest function entry cost a race frame ~1.5%.
static std::atomic<bool> stack_dump_requested{false};
// SFR_PROFILE_AFTER=N: both profilers ignore everything before the Nth present.
static const uint32_t profile_after = [] {
    const char* t = std::getenv("SFR_PROFILE_AFTER");
    return t ? uint32_t(std::strtoul(t, nullptr, 10)) : 0u;
}();
// SFR_SAMPLE_PROFILE: the most recently entered guest function, sampled every millisecond.
static std::atomic<uint64_t> profile_address{0};  // guest thread id << 32 | address
#ifdef _WIN32
// SFR_HOST_PROFILE: host thread that last entered a guest function, sampled
// for its instruction pointer (resolve the offsets with sfr_cpu_diagnostic.map).
static std::atomic<HANDLE> profile_host_thread{nullptr};
// SFR_MAIN_PROFILE: the same, but always one guest thread (the title's main
// thread, or guest SFR_PROFILE_GUEST), and without turning the per-entry
// diagnostics on.
static std::atomic<HANDLE> guest_host_threads[64]{};
static HANDLE own_thread_handle() {
    HANDLE handle = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0);
    return handle;
}
#endif
static int32_t main_thread_priority = 0;  // guest base priority increment of the main thread
// Thrown by ExTerminateThread and caught at the worker entry (not an error).
struct GuestThreadExit { uint32_t code; };
static thread_local uint64_t thread_calls = 0;
static uint64_t calls = 0;
static std::unique_ptr<NativeInput> native_input;
static uint64_t input_queries = 0, hid_queries = 0, keystroke_queries = 0, critical_region_calls = 0, printf_calls = 0;
static uint32_t last_input_packet = 0;
static NativeInput& input() {
    if (!native_input)
        native_input = std::make_unique<NativeInput>(NativeInput::host([]() -> void* {
            return active_guest_graphics && active_guest_graphics->created()
                ? active_guest_graphics->presentation().window_handle() : nullptr;
        }, [] {
            // SFR_INPUT_AFTER_PRESENT=N starts the input script at the Nth present.
            static const uint32_t after = [] {
                const char* t = std::getenv("SFR_INPUT_AFTER_PRESENT");
                return t ? uint32_t(std::strtoul(t, nullptr, 10)) : 0u;
            }();
            // SFR_INPUT_PRESENT_CLOCK=1 counts script seconds as 60 presents,
            // so a script does not depend on how fast the host runs the title.
            static const bool present_clock = std::getenv("SFR_INPUT_PRESENT_CLOCK") != nullptr;
            if (present_clock) {
                const uint64_t presents = present_count.load();
                return presents < after ? -1.0 : double(presents - after) / 60.0;
            }
            static std::optional<std::chrono::steady_clock::time_point> started;
            if (!started && present_count.load() >= after) started = std::chrono::steady_clock::now();
            return started ? std::chrono::duration<double>(std::chrono::steady_clock::now() - *started).count() : -1.0;
        }));
    return *native_input;
}
// Optional coverage: SFR_FUNCTION_TRACE names a file that receives every
// entered guest function address, one per line, when the diagnostic ends.
// ENTER lines stop after the first 100 calls, so they cannot show coverage.
constexpr uint32_t trace_base = 0x82000000, trace_limit = 0x83000000;
static std::unique_ptr<std::atomic<uint32_t>[]> entered_functions;
static void trace_entry(uint32_t address) {
    if (!entered_functions || address < trace_base || address >= trace_limit) return;
    const uint32_t index = (address - trace_base) / 4;
    entered_functions[index / 32].fetch_or(1u << (index % 32), std::memory_order_relaxed);
}
static void write_function_trace() {
    const char* path = std::getenv("SFR_FUNCTION_TRACE");
    if (!path || !entered_functions) return;
    std::ofstream out(path);
    for (uint32_t index = 0; index < (trace_limit - trace_base) / 4; ++index)
        if (entered_functions[index / 32].load(std::memory_order_relaxed) & (1u << (index % 32)))
            out << "0x" << std::hex << trace_base + index * 4 << '\n';
}
// The verified sampler boot now exceeds the old100000-call ceiling during
// subsequent original initialization. Keep a bounded diagnostic and watchdog.
// SFR_CALL_BUDGET / SFR_WATCHDOG_SECONDS raise these bounds for longer runs;
// the defaults keep the diagnostic and its tests bounded.
static uint64_t limit_from_environment(const char* name, uint64_t fallback) {
    const char* text = std::getenv(name);
    if (!text || !*text) return fallback;
    char* end = nullptr;
    const auto value = std::strtoull(text, &end, 10);
    return end && !*end && value ? value : fallback;
}
static const uint64_t call_budget = limit_from_environment("SFR_CALL_BUDGET", 2000000);
// SFR_TRACE_IMPORTS=0: skip the per-call lines of the imports a race calls
// hundreds of times a frame -- critical sections, event sets and waits, TLS
// lookups, thread suspension -- about 1500 lines a frame nobody reads while
// playing. On by default: the audits and tests read them.
static const bool trace_imports = [] {
    const char* const text = std::getenv("SFR_TRACE_IMPORTS");
    return !text || *text != '0';
}();
// SFR_GUEST_REACH=1 names, once per guest thread, each function and import
// a thread other than the main one reaches: what running it beside the main
// thread would have to make safe.
static const bool guest_reach = [] {
    const char* const text = std::getenv("SFR_GUEST_REACH");
    return text && *text != '0';
}();

std::atomic<uint64_t> main_gpu_wait_ns{0};
std::atomic<uint32_t> watch_word{[] {
    const char* t = std::getenv("SFR_WATCH_WORD");
    return t ? uint32_t(std::strtoul(t, nullptr, 16)) : 0u;
}()};
static const uint64_t watchdog_seconds = limit_from_environment("SFR_WATCHDOG_SECONDS", 10);
static XexModule* executable_module = nullptr;
static NativeModules* native_modules = nullptr;
static SystemConfig* system_config = nullptr;
static const GameRegion* game_region = nullptr;
static VirtualMemory* virtual_memory = nullptr;
static MemoryStatistics* memory_statistics = nullptr;
static PhysicalMemory* physical_memory = nullptr;
static ImageProtection* image_protection = nullptr;
static VideoGlobals* video_globals = nullptr;
static ThreadLocalStorage* thread_local_storage = nullptr;
static GuestClock* active_clock = nullptr;
static std::atomic<uint64_t> time_base_reads{0};
static uint64_t conditional_stores = 0;
static uint64_t vector_loads = 0, vector_stores = 0;
static uint64_t cache_block_zeroes = 0;
struct CopyObservation {
    bool active = false;
    uint32_t destination = 0, source = 0, stack = 0;
    std::array<uint8_t, 4096> source_bytes{};
};
static thread_local CopyObservation dds_copy;
struct RendererInitializationObservation {
    bool active = false;
    std::array<uint8_t, 124> parameters{};
    bool matrix_active = false;
    uint32_t matrix_source = 0, matrix_stack = 0;
    std::array<uint8_t, 64> matrix{};
    bool float_table_active = false;
    uint32_t float_table_stack = 0;
};
static thread_local RendererInitializationObservation renderer_initialization;
struct TextureTransferObservation {
    bool active = false;
    uint32_t stack = 0, surface = 0, resource = 0;
};
static thread_local TextureTransferObservation texture_transfer;
struct SharedSurfaceReleaseObservation {
    uint32_t resource = 0, heap_stack = 0;
    bool heap_active = false;
};
static thread_local SharedSurfaceReleaseObservation shared_surface_release;
struct TextureCreateObservation {
    bool active = false;
    uint32_t stack = 0, caller = 0, output = 0;
};
static thread_local TextureCreateObservation texture_create;
struct UnselectedUserObservation {
    bool active = false, reset_entered = false, converted = false;
    uint32_t index = 0, manager = 0, record = 0, reset_virtual = 0;
};
static thread_local UnselectedUserObservation unselected_user;
static thread_local bool sampler_inline_mip_pending = false;
static NativeWinsock* native_winsock = nullptr;
static NativeNotifications* native_notifications = nullptr;
static NotificationPlacement* notification_placement = nullptr;
static AssetFiles* asset_files = nullptr;
static GuestFiles* guest_files = nullptr;
static ContentFiles* content_files = nullptr;  // saves: mounted content packages
static GuestAsyncFiles* guest_async_files = nullptr;
static NativeSyncObjects* native_sync_objects = nullptr;
// Kernel dispatcher objects (KEVENT/KSEMAPHORE) live in guest memory and are
// initialized inline by the title. Each maps, on first kernel use, to a native
// object created from its DISPATCHER_HEADER: Type at +0 (0 notification event,
// 1 synchronization event, 5 semaphore), SignalState at +4, semaphore Limit at +16.
static struct { uint32_t routine, context; } audio_client{};
static std::atomic<uint64_t> audio_frames_requested{0}, audio_frames_submitted{0};
static uint32_t audio_pump_handle = 0;
static std::unordered_map<uint32_t, uint32_t> dispatcher_objects;
static std::atomic<uint64_t> dispatcher_waits{0};
// Guards dispatcher_objects and the guest DISPATCHER_HEADER signal states the
// Ke* imports keep, which guests running beside the permit update too.
static std::recursive_mutex dispatcher_mutex;
static uint32_t dispatcher_handle(uint32_t object) {
    std::lock_guard lock(dispatcher_mutex);
    if (const auto found = dispatcher_objects.find(object); found != dispatcher_objects.end()) return found->second;
    if (!object || (object & 3)) throw RuntimeStop("dispatcher-object", object, "invalid dispatcher object pointer");
    active_memory->check(object, 20);
    const auto type = active_memory->load<uint8_t>(object);
    const auto state = static_cast<int32_t>(active_memory->load<uint32_t>(uint64_t(object) + 4));
    NativeSyncObjects::CreateResult created{};
    if (type == 0 || type == 1)
        created = native_sync_objects->create_event(type == 0, state != 0);
    else if (type == 5)
        created = native_sync_objects->create_semaphore(state, static_cast<int32_t>(active_memory->load<uint32_t>(uint64_t(object) + 16)));
    else
        throw RuntimeStop("dispatcher-object", object, "unsupported dispatcher object type " + std::to_string(type));
    if (created.status) throw RuntimeStop("dispatcher-object", object, "native dispatcher object creation failed");
    dispatcher_objects.emplace(object, created.handle);
    std::cerr << "NATIVE_DISPATCHER_OBJECT object=0x" << std::hex << object << std::dec << " type=" << int(type)
              << " state=" << state << " handle=0x" << std::hex << created.handle << std::dec << '\n';
    return created.handle;
}
static GuestSyncObjects* guest_sync_objects = nullptr;
static GuestThreads* guest_threads = nullptr;
static NativeGraphics* native_graphics = nullptr;
// The diagnostic hosts one user process. Process switching is not implemented.
static constexpr uint32_t user_process_type = 1;
static constexpr uint32_t diagnostic_pcr = 0x70000000;
static constexpr uint32_t diagnostic_thread = diagnostic_pcr + 0xab0 + 0x100;
static GuestExecution* execution = nullptr;
static GuestCriticalSections* critical_sections = nullptr;
static thread_local GuestExecution::Lease* execution_permit = nullptr;
static thread_local const PPCContext* current_context = nullptr;
static thread_local uint32_t current_pcr = diagnostic_pcr, current_thread = diagnostic_thread, current_id = 1;
static thread_local uint32_t current_tls = ThreadLocalStorage::static_address, current_tls_dynamic = 0;

// SFR_PARALLEL_WORKER=1: the title's job worker (thread entry 0x8222E008)
// runs its guest code beside the permit's owner, as it would on a console
// core of its own, instead of taking turns with the main thread. It holds
// the permit only where it reaches host state: imports (bar critical
// sections, which have their own lock), our hooks, and computed memory
// words (GuestMemory::slow_access_hook); it reads the memory layout as a
// GuestMemory::concurrent_reader. Its trips to the permit are counted in
// PARALLEL_STATS lines.
// SFR_PARALLEL_WORKER=all does the same for every guest thread but the
// main thread and the audio pump. That runs threads the title pinned to one
// hardware thread at the same time, which the console never does: a race
// start then drained a lock-free queue into a pure virtual call (R6025).
// SFR_PARALLEL_WORKER=cores keeps the console's rule: a thread whose
// processor (PCR+0x10C) is 1-5 runs detached but holds that core's permit
// (core_executions, time-sliced like the global one) while it runs guest
// code; threads on processor 0, like the main thread, stay on the global
// permit.
enum class ParallelGuests { none, job_worker, all, cores };
static const ParallelGuests parallel_worker = [] {
    const char* const text = std::getenv("SFR_PARALLEL_WORKER");
    if (!text || *text == '0') return ParallelGuests::none;
    const std::string_view mode(text);
    return mode == "all" ? ParallelGuests::all : mode == "cores" ? ParallelGuests::cores : ParallelGuests::job_worker;
}();
static constexpr unsigned guest_processors = 6;
static std::array<std::unique_ptr<GuestExecution>, guest_processors> core_executions;
static thread_local std::unique_ptr<GuestExecution::Lease> core_permit;
static thread_local unsigned core_index = 0;
static constexpr uint32_t parallel_worker_entry = 0x8222E008;
static thread_local bool parallel_guest = false;
// Attached inside a hook: the guest stack pointer at the outermost hook's
// entry. The hook has returned once a function is entered above it.
static thread_local uint32_t parallel_hook_sp = 0;
// Attached for a slow memory check: detach at the next function entry.
static thread_local bool parallel_detach_at_entry = false;
static std::atomic<uint64_t> parallel_attaches[3]{};  // import, hook, memory
static void parallel_attached(int reason) {
    parallel_attaches[reason].fetch_add(1, std::memory_order_relaxed);
    static thread_local uint64_t count = 0;
    if (++count % 20000 == 0)
        std::cerr << "PARALLEL_STATS imports=" << parallel_attaches[0] << " hooks=" << parallel_attaches[1]
                  << " memory=" << parallel_attaches[2] << char(10);
}

static void parallel_slow_access(uint64_t address) {
    if (!execution_permit || !execution_permit->detached()) return;
    static const bool trace = [] { const char* t = std::getenv("SFR_PARALLEL_TRACE"); return t && *t != '0'; }();
    if (trace) {
        static thread_local std::unordered_map<uint32_t, uint64_t> pages;
        const uint64_t count = ++pages[uint32_t(address >> 12)];
        if ((count & (count - 1)) == 0 && count >= 256)
            std::cerr << "PARALLEL_SLOW page=0x" << std::hex << (address >> 12) << "000 address=0x" << address
                      << std::dec << " count=" << count << " function=" << current_function << char(10);
    }
    execution_permit->attach();
    parallel_attached(2);
    parallel_detach_at_entry = true;
}

static void parallel_function_entry(const PPCContext& ctx, uint32_t address) {
    if (execution_permit->detached()) {
        if (!is_hook(address)) return;
        execution_permit->attach();
        parallel_attached(1);
        parallel_hook_sp = ctx.r1.u32;
    } else if (parallel_hook_sp) {
        if (is_hook(address)) parallel_hook_sp = (std::max)(parallel_hook_sp, ctx.r1.u32);
        else if (ctx.r1.u32 > parallel_hook_sp) {
            parallel_hook_sp = 0;
            execution_permit->detach();
        }
    } else if (parallel_detach_at_entry) {
        parallel_detach_at_entry = false;
        if (is_hook(address)) parallel_hook_sp = ctx.r1.u32;
        else execution_permit->detach();
    }
}

static std::unordered_set<uint32_t>& hooks() {
    static std::unordered_set<uint32_t> set;
    return set;
}
bool register_hook(const char* name) {
    hooks().insert(uint32_t(std::strtoul(name + 4, nullptr, 16)));
    return true;
}
bool is_hook(uint32_t address) { return hooks().contains(address); }

// The guest call chain from a stack pointer: each frame's first word links
// to the caller's frame, and the saved LR sits 8 bytes below that frame.
static std::string guest_back_chain(uint32_t frame) {
    std::ostringstream text;
    text << std::hex;
    try {
        for (int depth = 0; depth < 24 && frame; ++depth) {
            const uint32_t caller = active_memory->load<uint32_t>(frame);
            if (!caller || caller <= frame) break;
            text << "0x" << active_memory->load<uint32_t>(uint64_t(caller) - 8) << ',';
            frame = caller;
        }
    } catch (...) { text << "unreadable"; }
    return text.str();
}

void guest_checkpoint_permit() {
    checkpoint_countdown = 31;
    if (!execution_permit) throw std::logic_error("guest instruction without execution permit");
    // Validates ownership and cancellation before shared memory access, and
    // hands off only outside a reservation.
    const bool handoff = !active_memory->has_reservation();
    execution_permit->checkpoint(handoff);
    // A core's threads take turns only while detached from the global permit
    // (an attached thread holds both for a short import or hook).
    if (core_permit) core_permit->checkpoint(handoff && execution_permit->detached());
}

[[noreturn]] void unsupported_function(PPCContext&, const char* name, uint32_t address, const char* reason) {
    guest_checkpoint();
    throw RuntimeStop("unsupported-function", address, std::string(name) + ": " + reason);
}
uint64_t read_time_base() {
    if (!active_clock) throw RuntimeStop("clock-context", 0, "guest monotonic clock is not initialized");
    const uint64_t ticks = active_clock->time_base();
    if (++time_base_reads <= 4)
        std::cerr << "TIME_BASE ticks=" << ticks << " frequency=" << GuestClock::frequency << '\n';
    return ticks;
}
static void check_reservation_context(const PPCContext& ctx) {
    if (!execution_permit || current_context != &ctx || !active_memory || ctx.r13.u32 != current_pcr ||
        active_memory->load<uint32_t>(uint64_t(current_pcr) + 0x100) != current_thread ||
        active_memory->load<uint32_t>(uint64_t(current_thread) + 0x14c) != current_id)
        throw RuntimeStop("thread-context", ctx.r13.u32, "guest context does not match the executing native thread");
}
// Host-driven system threads (argument selects the loop in the thread entry).
constexpr uint32_t system_thread_audio = 0;

// Creates and starts a system thread with its own PCR, TLS and stack, like
// the console's kernel threads; returns its handle.
static uint32_t start_system_thread(PPCContext& ctx, uint32_t kind) {
    check_reservation_context(ctx);
    const uint32_t output = ctx.r1.u32 - 0x100;  // below the stack pointer, unused
    const GuestThreads::Request request{output, 0x10000, 0, 0, 0, kind, 1,
        active_memory->load<uint8_t>(uint64_t(ctx.r13.u32) + 0x10C), true};
    guest_threads->create(request);
    const uint32_t handle = active_memory->load<uint32_t>(output);
    const auto resumed = guest_threads->resume(handle, 0);
    if (!resumed.status && resumed.started) {
        execution->wait_until_ready(resumed.id);
        execution_permit->renew_quantum();
    }
    return handle;
}

GamepadState nui_gamepad() {
    return input().current(0).value_or(GamepadState{});
}

static std::jthread nui_events;
void start_nui_skeleton_events(uint32_t event_handle) {
    std::shared_ptr<NativeSyncObjects::RetainedEvent> event = native_sync_objects->retain_event(event_handle);
    if (!event) throw RuntimeStop("nui-event", event_handle, "skeleton frame event is not an event");
    nui_events = std::jthread([event](std::stop_token stop) {
        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::microseconds(33333));
            event->signal();
        }
    });
}
void stop_nui_skeleton_events() {
    nui_events = {};
}

uint32_t load_reserved_word(PPCContext& ctx, uint64_t address) {
    check_reservation_context(ctx);
    return active_memory->load_reserved_word(address);
}
void store_conditional_word(PPCContext& ctx, uint64_t address, uint32_t value) {
    check_reservation_context(ctx);
    const bool success = active_memory->store_conditional_word(address, value);
    ctx.cr0.lt = 0;
    ctx.cr0.gt = 0;
    ctx.cr0.eq = success;
    ctx.cr0.so = ctx.xer.so;
    if (++conditional_stores <= 4)
        std::cerr << "STORE_CONDITIONAL address=0x" << std::hex << address << " value=0x" << value
                  << " success=" << std::dec << success << '\n';
}
void synchronize_resource_memory(PPCContext& ctx) {
    parallel_slow_access(ctx.r23.u32);
    check_reservation_context(ctx);
    if (active_memory->has_reservation())
        throw RuntimeStop("resource-coherency", ctx.r23.u32, "GPU synchronization during a live reservation");
    if (!physical_memory || !native_graphics || !native_graphics->initialized() ||
        !active_guest_graphics || !active_guest_graphics->created())
        throw RuntimeStop("resource-coherency", ctx.r23.u32, "native graphics owner is unavailable");
    const uint64_t stack = ctx.r1.u32;
    ResourceCoherencyRequest request{
        active_memory->load<uint32_t>(stack + 184), ctx.r26.u32, ctx.r30.u64,
        ctx.r27.u32, ctx.r20.u32, ctx.r21.u32, ctx.r22.u32, ctx.r23.u32, ctx.r24.u32,
        ctx.r25.u32, active_memory->load<uint32_t>(stack + 284)};
    synchronize_resource_memory(*active_memory, *physical_memory, request,
                                [] { native_graphics->wait_idle([] { return execution->stopped(); }); });
    execution_permit->checkpoint(false);
    // Virtual physical views: 0xE0000000 maps physical + 0x1000, the others mask.
    const uint32_t physical_address = request.address >= 0xE0000000u ? request.address - 0xE0000000u + 0x1000u
                                                                      : request.address & 0x1FFFFFFFu;
    active_guest_graphics->renderer().invalidate(physical_address, request.size);
    if (texture_transfer.active) texture_transfer.resource = request.resource;
    std::cerr << "NATIVE_RESOURCE_COHERENCY resource=0x" << std::hex << request.resource
              << " address=0x" << request.address << " size=0x" << request.size
              << " mask=0x" << request.mask << " caller=0x" << request.caller << std::dec << " operation=" << request.operation
              << " ownership=cpu-only queue_completed=1\n";
}
uint64_t load_reserved_doubleword(PPCContext& ctx, uint64_t address) {
    check_reservation_context(ctx);
    return active_memory->load_reserved_doubleword(address);
}
void store_conditional_doubleword(PPCContext& ctx, uint64_t address, uint64_t value) {
    check_reservation_context(ctx);
    const bool success = active_memory->store_conditional_doubleword(address, value);
    ctx.cr0.lt = 0;
    ctx.cr0.gt = 0;
    ctx.cr0.eq = success;
    ctx.cr0.so = ctx.xer.so;
    static uint64_t doubleword_stores = 0; // Accessed only under the guest execution permit.
    if (++doubleword_stores <= 4)
        std::cerr << "STORE_CONDITIONAL_DOUBLEWORD address=0x" << std::hex << address << " value=0x" << value
                  << " success=" << std::dec << success << '\n';
}
// The sixteen bytes of an aligned vector reversed (the register holds byte 15
// of memory in element 0), in one shuffle where SSSE3 exists.
static inline void reverse_vector(const uint8_t* from, uint8_t* to) {
#if defined(__SSSE3__)
    const __m128i order = _mm_setr_epi8(15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(to),
                     _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(from)), order));
#else
    for (unsigned i = 0; i < 16; ++i) to[i] = from[15 - i];
#endif
}
void load_vector_memory(uint32_t address, uint8_t (&destination)[16]) {
    if (!active_memory) throw RuntimeStop("memory-context", address, "guest memory is not initialized");
    // Common case inline: an aligned vector in one fast page.
    if (const uint8_t* bytes = active_memory->fast_read(address & ~0xFu, 16)) {
        reverse_vector(bytes, destination);
        if (vector_loads < 4 && ++vector_loads)
            std::cerr << "VECTOR_LOAD address=0x" << std::hex << (address & ~0xFu) << std::dec << '\n';
        return;
    }
    const auto value = load_vector_memory(*active_memory, address);
    std::copy(value.begin(), value.end(), destination);
    if (++vector_loads <= 4)
        std::cerr << "VECTOR_LOAD address=0x" << std::hex << (address & ~0xFu) << std::dec << '\n';
}
void store_vector_memory(uint32_t address, const uint8_t (&source)[16]) {
    if (!active_memory) throw RuntimeStop("memory-context", address, "guest memory is not initialized");
    if (uint8_t* bytes = active_memory->fast_write(address & ~0xFu, 16)) {
        reverse_vector(source, bytes);
        if (vector_stores < 4 && ++vector_stores)
            std::cerr << "VECTOR_STORE address=0x" << std::hex << (address & ~0xFu) << std::dec << '\n';
        return;
    }
    VectorBytes value;
    std::copy(std::begin(source), std::end(source), value.begin());
    store_vector_memory(*active_memory, address, value);
    if (++vector_stores <= 4)
        std::cerr << "VECTOR_STORE address=0x" << std::hex << (address & ~0xFu) << std::dec << '\n';
}
void store_vector_word(uint32_t address, const uint8_t (&source)[16]) {
    if (!active_memory) throw RuntimeStop("memory-context", address, "guest memory is not initialized");
    VectorBytes value;
    std::copy(std::begin(source), std::end(source), value.begin());
    store_vector_word(*active_memory, address, value);
}
void zero_cache_block(uint32_t address) {
    if (!active_memory) throw RuntimeStop("memory-context", address, "guest memory is not initialized");
    active_memory->zero_cache_block(address);
    if (++cache_block_zeroes <= 4)
        std::cerr << "CACHE_BLOCK_ZERO effective=0x" << std::hex << address
                  << " aligned=0x" << (address & ~uint32_t{31}) << std::dec << " bytes=32\n";
}
void load_vector_left(uint32_t address, uint8_t (&destination)[16]) {
    if (!active_memory) throw RuntimeStop("memory-context", address, "guest memory is not initialized");
    const auto value = load_vector_left(*active_memory, address);
    std::copy(value.begin(), value.end(), destination);
}
void load_vector_right(uint32_t address, uint8_t (&destination)[16]) {
    if (!active_memory) throw RuntimeStop("memory-context", address, "guest memory is not initialized");
    const auto value = load_vector_right(*active_memory, address);
    std::copy(value.begin(), value.end(), destination);
}
void store_vector_left(uint32_t address, const uint8_t (&source)[16]) {
    if (!active_memory) throw RuntimeStop("memory-context", address, "guest memory is not initialized");
    VectorBytes value;
    std::copy(std::begin(source), std::end(source), value.begin());
    store_vector_left(*active_memory, address, value);
}
void store_vector_right(uint32_t address, const uint8_t (&source)[16]) {
    if (!active_memory) throw RuntimeStop("memory-context", address, "guest memory is not initialized");
    VectorBytes value;
    std::copy(std::begin(source), std::end(source), value.begin());
    store_vector_right(*active_memory, address, value);
}
void zero_cache_line(uint32_t address) {
    if (!active_memory) throw RuntimeStop("memory-context", address, "guest memory is not initialized");
    active_memory->zero_cache_line(address);
}
// SFR_WAIT_GRAPH=1: every five seconds, the main thread's time blocked on
// each wait object and which guests signalled it: what a frame waits for.
static const bool wait_graph = [] {
    const char* const text = std::getenv("SFR_WAIT_GRAPH");
    return text && *text != '0';
}();
struct WaitGraph {
    std::mutex mutex;
    std::unordered_map<uint32_t, double> main_ms;
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t>> setters;
    std::chrono::steady_clock::time_point reported = std::chrono::steady_clock::now();
};
static WaitGraph& wait_graph_state() {
    static WaitGraph graph;
    return graph;
}
static void wait_graph_signal(uint32_t object) {
    if (!wait_graph) return;
    auto& graph = wait_graph_state();
    std::lock_guard lock(graph.mutex);
    ++graph.setters[object][current_id];
}
static void wait_graph_wait(uint32_t object, std::chrono::steady_clock::time_point started) {
    if (!wait_graph || current_id != 1) return;
    auto& graph = wait_graph_state();
    std::lock_guard lock(graph.mutex);
    const auto now = std::chrono::steady_clock::now();
    graph.main_ms[object] += std::chrono::duration<double, std::milli>(now - started).count();
    if (now - graph.reported < std::chrono::seconds(5)) return;
    std::vector<std::pair<double, uint32_t>> top;
    for (const auto& [waited, ms] : graph.main_ms) top.push_back({ms, waited});
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < top.size() && i < 4; ++i) {
        std::cerr << "WAIT_GRAPH object=0x" << std::hex << top[i].second << std::dec << " main_ms=" << top[i].first << " setters=";
        for (const auto& [id, count] : graph.setters[top[i].second]) std::cerr << id << ':' << count << ',';
        std::cerr << char(10);
    }
    graph.main_ms.clear();
    graph.setters.clear();
    graph.reported = now;
}

// Runs a blocking wait for the current guest: through the permit's
// run_blocking when it holds the permit, and otherwise (a detached guest)
// releasing only the core it holds, so a detached guest waits without first
// queueing for the permit.
template<class Operation> static void block_guest(Operation&& operation) {
    if (!execution_permit->detached()) {
        execution_permit->run_blocking(std::forward<Operation>(operation));
        return;
    }
    if (core_permit) core_permit->run_blocking([&](std::stop_token stop) { operation(stop); });
    else operation(execution->stop_token());
}

void wait_without_permit(void (*wait)(void*), void* argument) {
    if (!execution_permit) { wait(argument); return; }
    block_guest([&](std::stop_token) { wait(argument); });
}

// Who resumed a created-suspended thread, and how many times that resumer
// had blocked then (GuestExecution::wait_for_block), by thread handle.
struct ThreadResumer { uint32_t guest_id; uint64_t blocks; };
static std::mutex thread_resumers_lock;
static std::unordered_map<uint32_t, ThreadResumer> thread_resumers;

static void dispatch_import_owned(PPCContext& ctx, const char* name, uint32_t address);
void dispatch_import(PPCContext& ctx, const char* name, uint32_t address) {
    // SFR_MAIN_IMPORT_TIME=1: every five seconds, the main thread's time in
    // each import (waits included) since the last report.
    static const bool main_import_time = [] {
        const char* const text = std::getenv("SFR_MAIN_IMPORT_TIME");
        return text && *text != '0';
    }();
    if (main_import_time && current_id == 1) {
        static std::unordered_map<const char*, double> spent;
        static auto reported = std::chrono::steady_clock::now();
        const auto started = std::chrono::steady_clock::now();
        dispatch_import_owned(ctx, name, address);
        const auto now = std::chrono::steady_clock::now();
        spent[name] += std::chrono::duration<double, std::milli>(now - started).count();
        if (now - reported > std::chrono::seconds(5)) {
            std::vector<std::pair<double, const char*>> top;
            for (const auto& [import, ms] : spent) top.push_back({ms, import});
            std::sort(top.rbegin(), top.rend());
            std::cerr << "MAIN_IMPORT_TIME seconds=" << std::chrono::duration<double>(now - reported).count();
            for (size_t i = 0; i < top.size() && i < 6; ++i) std::cerr << ' ' << top[i].second + 7 << '=' << top[i].first;
            std::cerr << char(10);
            spent.clear();
            reported = now;
        }
        return;
    }
    if (!parallel_guest || !execution_permit->detached()) return dispatch_import_owned(ctx, name, address);
    // Imports a detached guest runs as it is: critical sections keep their
    // own lock (only a contended enter attaches, to wait), a TLS value lives
    // in the calling thread's own bank, and the process type is a constant.
    // KeTlsGetValue alone brought detached guests back to the permit some
    // four hundred thousand times in a race's first minute.
    // Waits, event signals and sleeps too: the sync objects keep their own
    // lock (and dispatcher_mutex the guest-side state), and a wait releases
    // only the core (block_guest). A thread handle's wait reads the thread
    // registry, so it still takes the permit.
    const bool permit_free = address == 0x82ACB4AC || address == 0x82ACB4BC ||  // Rtl{Enter,Leave}CriticalSection
        address == 0x82ACBCAC || address == 0x82ACBCBC ||                        // KeTls{Get,Set}Value
        address == 0x82ACB59C ||                                                 // KeGetCurrentProcessType
        address == 0x82ACB86C || address == 0x82ACC1CC ||                        // KeWaitFor{Single,Multiple}Object(s)
        address == 0x82ACB78C ||                                                 // KeDelayExecutionThread
        address == 0x82ACB87C || address == 0x82ACB9DC ||                        // Ke{Set,Reset}Event
        address == 0x82ACC2EC ||                                                 // KeReleaseSemaphore
        address == 0x82ACB5EC || address == 0x82ACB6CC ||                        // Nt{Set,Clear}Event
        address == 0x82ACC18C ||                                                 // XAudioGetVoiceCategoryVolume (a constant)
        (address == 0x82ACB63C && !GuestThreads::is_handle_range(ctx.r3.u32));  // NtWaitForSingleObjectEx
    if (!permit_free) {
        execution_permit->attach();
        parallel_attached(0);
        // Holding the permit: which imports bring detached guests back to it.
        static std::unordered_map<const char*, uint64_t> by_import;
        static uint64_t attaches = 0;
        ++by_import[name];
        if (++attaches % 50000 == 0) {
            std::vector<std::pair<uint64_t, const char*>> top;
            for (const auto& [import, count] : by_import) top.push_back({count, import});
            std::sort(top.rbegin(), top.rend());
            std::cerr << "PARALLEL_IMPORTS";
            for (size_t i = 0; i < top.size() && i < 8; ++i) std::cerr << ' ' << top[i].second + 7 << '=' << top[i].first;
            std::cerr << char(10);
        }
    }
    dispatch_import_owned(ctx, name, address);
    if (!execution_permit->detached() && !parallel_hook_sp && !parallel_detach_at_entry) execution_permit->detach();
}
// Completes an XOVERLAPPED at once: InternalLow = result, InternalHigh =
// length, extended error 0, then its event (if any) is set. Returns
// ERROR_IO_PENDING, what the asynchronous XAM call reports.
static uint32_t complete_overlapped(uint32_t overlapped, uint32_t result, uint32_t length = 0) {
    active_memory->check_write(overlapped, 0x1C);
    if (active_memory->load<uint32_t>(uint64_t(overlapped) + 0x10))
        throw RuntimeStop("xam-overlapped", overlapped, "overlapped completion routines are unsupported");
    active_memory->store<uint32_t>(overlapped, result);
    active_memory->store<uint32_t>(uint64_t(overlapped) + 4, length);
    active_memory->store<uint32_t>(uint64_t(overlapped) + 0x18, 0);
    if (const uint32_t event = active_memory->load<uint32_t>(uint64_t(overlapped) + 0xC))
        native_sync_objects->set_event(event);
    return 997;
}

static void dispatch_import_owned(PPCContext& ctx, const char* name, uint32_t address) {
    guest_checkpoint();
    if (guest_reach && current_id != 1) {
        static thread_local std::unordered_set<uint32_t> reached;
        if (reached.insert(address).second)
            std::cerr << "GUEST_REACH guest=" << current_id << " import=" << name << '\n';
    }
    if (active_memory && active_memory->has_reservation())
        throw RuntimeStop("reservation-interference", address, "import call during live reservation");
    if (address == 0x82ACB6BC && std::string_view(name) == "__imp__RtlMultiByteToUnicodeN" &&
        ctx.lr == 0x824D0E3C && unselected_user.active) {
        const uint64_t record = unselected_user.record;
        bool names_clear = true, profile_clear = true;
        for (uint32_t i = 0; i < 48; ++i)
            names_clear &= active_memory->load<uint8_t>(record + 7020 + i) == 0;
        for (uint32_t i = 0; i < 1000; ++i)
            profile_clear &= active_memory->load<uint8_t>(record + 7072 + i) == 0;
        const uint32_t state = active_memory->load<uint32_t>(record + 7068);
        const uint32_t selected = active_memory->load<uint32_t>(uint64_t(unselected_user.manager) + 32520);
        if (!unselected_user.reset_entered || !unselected_user.reset_virtual || !names_clear || !profile_clear ||
            state != 0 || (selected != 0xffffffff && (selected >= 4 || !profile_for(selected))) ||
            ctx.r3.u32 != record + 7036 || ctx.r4.u32 != 32 ||
            ctx.r5.u32 != 0 || ctx.r6.u32 != record + 7020 || ctx.r7.u32 != 1)
            throw RuntimeStop("original-user-reset", record, "original unselected-user reset evidence differs");
        std::cerr << "ORIGINAL_UNSELECTED_USER index=" << unselected_user.index << " record=0x" << std::hex << record
                  << " state=0 selected=-1 names_cleared=1 profile_cleared=1 reset_virtual=0x"
                  << unselected_user.reset_virtual << std::dec << '\n';
    }
    if (address == 0x82ACB6BC && std::string_view(name) == "__imp__RtlMultiByteToUnicodeN") {
        const uint32_t output = ctx.r3.u32, capacity = ctx.r4.u32, written_output = ctx.r5.u32;
        const uint32_t input = ctx.r6.u32, input_bytes = ctx.r7.u32;
        ctx.r3.u64 = multibyte_to_unicode_ascii(*active_memory, output, capacity, written_output, input, input_bytes);
        const uint32_t output_bytes = 2 * (std::min)(input_bytes, capacity / 2);
        if (unselected_user.active && ctx.lr == 0x824D0E3C)
            unselected_user.converted = true;
        std::cerr << "NATIVE_MULTIBYTE_UNICODE input=0x" << std::hex << input << std::dec
                  << " input_bytes=" << input_bytes << " output=0x" << std::hex << output << std::dec
                  << " capacity=" << capacity << " written_output=0x" << std::hex << written_output << std::dec
                  << " output_bytes=" << output_bytes << " status=0x" << std::hex << ctx.r3.u32
                  << " lr=0x" << ctx.lr << std::dec << " encoding=ascii-subset\n";
        return;
    }
    if (address == 0x82ACB26C && std::string_view(name) == "__imp__XNotifyPositionUI" && notification_placement) {
        notification_placement->set(ctx.r3.u32);
        const auto& selected = *notification_placement->selected();
        const auto horizontal = selected.horizontal == NotificationPlacement::Horizontal::left ? "left" :
            selected.horizontal == NotificationPlacement::Horizontal::right ? "right" : "center";
        const auto vertical = selected.vertical == NotificationPlacement::Vertical::top ? "top" :
            selected.vertical == NotificationPlacement::Vertical::bottom ? "bottom" : "center";
        std::cerr << "NATIVE_NOTIFICATION_POSITION flags=0x" << std::hex << selected.flags
                  << " horizontal=" << horizontal << " vertical=" << vertical
                  << " lr=0x" << ctx.lr << std::dec << " state=retained-placement\n";
        if (ctx.lr == 0x82232E48) {
            const uint32_t saved = active_memory->load<uint32_t>(uint64_t(ctx.r30.u32) + 32480);
            if (!native_notifications || !native_notifications->owns(saved))
                throw RuntimeStop("notification-original-storage", saved, "original constructor did not retain its listener");
            std::cerr << "ORIGINAL_NOTIFICATION_HANDLE owner=0x" << std::hex << ctx.r30.u32
                      << " offset=0x7ee0 handle=0x" << saved << std::dec << '\n';
        }
        return; // Void ABI; retaining a placement does not publish a UI event.
    }
    if (address == 0x82ACB40C && std::string_view(name) == "__imp__XamNotifyCreateListener" && native_notifications) {
        const uint64_t mask = ctx.r3.u64;
        const uint32_t maximum_version = ctx.r4.u32;
        const uint32_t handle = native_notifications->create(mask, maximum_version);
        ctx.r3.u64 = handle;
        std::cerr << "NATIVE_NOTIFICATION_CREATE mask=0x" << std::hex << mask << std::dec
                  << " maximum_version=" << maximum_version << " handle=0x" << std::hex << handle
                  << " queued=0 lr=0x" << ctx.lr << std::dec << " backend=windows-event\n";
        return;
    }
    if (address == 0x82ACB25C && std::string_view(name) == "__imp__XNotifyGetNext" && native_notifications) {
        const uint32_t handle = ctx.r3.u32, match = ctx.r4.u32;
        const uint32_t id_output = ctx.r5.u32, parameter_output = ctx.r6.u32;
        const bool found = native_notifications->get_next(handle, match, id_output, parameter_output);
        ctx.r3.u64 = found;
        std::cerr << "NATIVE_NOTIFICATION_NEXT handle=0x" << std::hex << handle << " match=0x" << match
                  << " id_output=0x" << id_output << " parameter_output=0x" << parameter_output
                  << " found=" << found << " lr=0x" << ctx.lr;
        if (found)
            std::cerr << " id=0x" << active_memory->load<uint32_t>(id_output)
                      << " parameter=0x" << (parameter_output ? active_memory->load<uint32_t>(parameter_output) : 0);
        std::cerr << std::dec << " backend=native-notification-queue\n";
        return;
    }
    if (address == 0x82ACBD5C && std::string_view(name) == "__imp__NetDll_WSAStartup" && native_winsock) {
        const uint32_t caller = ctx.r3.u32, output = ctx.r5.u32;
        const uint16_t version = static_cast<uint16_t>(ctx.r4.u32);
        const int result = native_winsock->startup(caller, version, output);
        ctx.r3.u64 = static_cast<uint32_t>(result);
        std::cerr << "NATIVE_WSA_STARTUP caller=" << caller << " requested=0x" << std::hex << version
                  << " output=0x" << output << " bytes=400 status=0x" << static_cast<uint32_t>(result);
        if (result == 0)
            std::cerr << " version=0x" << active_memory->load<uint16_t>(output)
                      << " high_version=0x" << active_memory->load<uint16_t>(uint64_t(output) + 2);
        std::cerr << std::dec << " acquisitions=" << native_winsock->acquisitions()
                  << " lr=0x" << std::hex << ctx.lr << std::dec << " backend=windows-winsock\n";
        return;
    }
    if (address == 0x82ACBEEC && std::string_view(name) == "__imp__XamContentOpenFile") {
        // XamContentOpenFile(user, root, path, flags, disposition*, license*, overlapped*).
        // No title update or content package is installed, so the file is absent.
        if (ctx.r9.u32) throw RuntimeStop("xam-content", ctx.r9.u32, "asynchronous content open is unsupported");
        const auto text = [](uint32_t address) {
            std::string value;
            for (uint32_t i = 0; i < 260 && active_memory->load<uint8_t>(uint64_t(address) + i); ++i)
                value += char(active_memory->load<uint8_t>(uint64_t(address) + i));
            return value;
        };
        std::cerr << "NATIVE_CONTENT_OPEN root=" << text(ctx.r4.u32) << " path=" << text(ctx.r5.u32)
                  << " status=0x2 lr=0x" << std::hex << ctx.lr << std::dec << " backend=no-content-packages\n";
        ctx.r3.u64 = 2;  // ERROR_FILE_NOT_FOUND
        return;
    }
    if (address == 0x82ACC16C && std::string_view(name) == "__imp__XamInputGetKeystrokeEx") {
        // XamInputGetKeystrokeEx(user_index*, flags, XINPUT_KEYSTROKE*). Keyboard
        // queries (no XINPUT_FLAG_GAMEPAD) have no device; gamepad keystrokes are
        // not synthesized yet, so report an empty queue.
        const uint32_t flags = ctx.r4.u32;
        const bool gamepad = (flags & 0xFF) == 0 || (flags & 1);
        ctx.r3.u64 = gamepad ? 0x10D2 : 0x48F;  // ERROR_EMPTY / ERROR_DEVICE_NOT_CONNECTED
        if (keystroke_queries++ == 0)
            std::cerr << "NATIVE_KEYSTROKE flags=0x" << std::hex << flags << " status=0x" << ctx.r3.u32
                      << " lr=0x" << ctx.lr << std::dec << " backend=no-keystroke-events\n";
        return;
    }
    if (address == 0x82ACC46C && std::string_view(name) == "__imp__HidReadKeys") {
        // No USB HID keyboard or chatpad: game input comes through XamInputGetState.
        // STATUS_DEVICE_NOT_CONNECTED is the status titles check for (Xenia notes).
        ctx.r3.u64 = 0xC000009D;
        if (hid_queries++ == 0)
            std::cerr << "NATIVE_HID_READ_KEYS status=0xc000009d lr=0x" << std::hex << ctx.lr << std::dec
                      << " backend=no-hid-keyboard\n";
        return;
    }
    if (address == 0x82ACC15C && std::string_view(name) == "__imp__XamInputSetState") {
        // XamInputSetState(user, flags, XINPUT_VIBRATION*): two BE16 motor speeds.
        const uint32_t user = ctx.r3.u32, flags = ctx.r4.u32, vibration = ctx.r5.u32;
        if (flags || !vibration)
            throw RuntimeStop("native-input", flags, "unsupported XamInputSetState flags or null vibration");
        active_memory->check(vibration, 4);
        const uint16_t left = active_memory->load<uint16_t>(vibration);
        const uint16_t right = active_memory->load<uint16_t>(uint64_t(vibration) + 2);
        ctx.r3.u64 = input().set_vibration(user, left, right);
        std::cerr << "NATIVE_VIBRATION user=" << user << " left=" << left << " right=" << right
                  << " status=0x" << std::hex << ctx.r3.u32 << std::dec << '\n';
        return;
    }
    if (address == 0x82ACC14C && std::string_view(name) == "__imp__XamInputGetState") {
        // XamInputGetState(user, flags, XINPUT_STATE*). Only gamepad queries
        // (flags 0 or XINPUT_FLAG_GAMEPAD) for users 0..3 with a real output.
        const uint32_t user = ctx.r3.u32, flags = ctx.r4.u32, output = ctx.r5.u32;
        if ((flags & ~1u) || !output)
            throw RuntimeStop("native-input", flags, "unsupported XamInputGetState flags or null state");
        ctx.r3.u64 = input().get_state(*active_memory, user, output);
        // Polled every frame: log only the first query and every state change.
        const uint32_t packet = ctx.r3.u32 == xinput_success ? active_memory->load<uint32_t>(output) : 0;
        if (input_queries++ == 0 || packet != last_input_packet) {
            std::cerr << "NATIVE_INPUT user=" << user << " status=0x" << std::hex << ctx.r3.u32;
            if (ctx.r3.u32 == xinput_success)
                std::cerr << " packet=" << std::dec << packet << " buttons=0x" << std::hex
                          << active_memory->load<uint16_t>(uint64_t(output) + 4);
            std::cerr << " lr=0x" << ctx.lr << std::dec << " backend=xinput+keyboard\n";
            last_input_packet = packet;
        }
        return;
    }
    if (address == 0x82ACB32C && std::string_view(name) == "__imp__XamUserGetSigninState") {
        const uint32_t index = ctx.r3.u32;
        ctx.r3.u64 = profile_signin_state(index);
        if (ctx.lr == 0x822344D8 && ctx.r3.u32 == 0) {
            unselected_user = {true, false, false, index, ctx.r30.u32, ctx.r31.u32, 0};
            entry_observed = true;
        }
        std::cerr << "NATIVE_USER_SIGNIN index=" << index << " state=" << ctx.r3.u32
                  << " lr=0x" << std::hex << ctx.lr << std::dec << " backend=local-profile\n";
        return;
    }
    if (address == 0x82ACB31C && std::string_view(name) == "__imp__XamUserGetName") {
        // XamUserGetName(user, char* buffer, size): the gamertag, terminated.
        const uint32_t user = ctx.r3.u32, buffer = ctx.r4.u32, size = ctx.r5.u32;
        const LocalProfile* profile = profile_for(user);
        if (!profile) {
            ctx.r3.u64 = 0x525;  // ERROR_NO_SUCH_USER
        } else {
            if (!buffer || !size) throw RuntimeStop("user-name", buffer, "XamUserGetName needs an output buffer");
            active_memory->check_write(buffer, size);
            const size_t count = std::min<size_t>(profile->name.size(), size - 1);
            for (size_t i = 0; i < count; ++i) active_memory->store<uint8_t>(uint64_t(buffer) + i, uint8_t(profile->name[i]));
            active_memory->store<uint8_t>(uint64_t(buffer) + count, 0);
            ctx.r3.u64 = 0;
        }
        std::cerr << "NATIVE_USER_NAME user=" << user << " status=0x" << std::hex << ctx.r3.u32 << std::dec
                  << " lr=0x" << std::hex << ctx.lr << std::dec << " backend=local-profile\n";
        return;
    }
    if (address == 0x82ACB35C && std::string_view(name) == "__imp__XamGetSystemVersion") {
        ctx.r3.u64 = NativeModules::compatibility_version;
        std::cerr << "XAM_SYSTEM_VERSION value=0x" << std::hex << ctx.r3.u32 << std::dec
                  << " target=2.0.12416.0 source=verified-game-abi\n";
        return;
    }
    if (address == 0x82ACB3FC && std::string_view(name) == "__imp__XamShowNuiMessageBoxUI") {
        // (tracking id, user, title, text, button count, buttons, focus, flags,
        // result* at sp+84, XOVERLAPPED* at sp+92). No system UI: the title's
        // focused button is chosen and the operation completes at once, as a
        // player confirming it (e.g. the voice-recognition language notice at
        // START). With no profiles, a "Sign in" choice would only reopen an
        // empty sign-in panel, so another button is chosen instead.
        const auto wide = [](uint32_t address) {
            std::string value;
            for (uint32_t i = 0; address && i < 256; ++i) {
                const uint16_t c = active_memory->load<uint16_t>(uint64_t(address) + i * 2);
                if (!c) break;
                value += c < 128 ? char(c) : '?';
            }
            return value;
        };
        const uint32_t result = active_memory->load<uint32_t>(uint64_t(ctx.r1.u32) + 84);
        const uint32_t overlapped = active_memory->load<uint32_t>(uint64_t(ctx.r1.u32) + 92);
        const uint32_t count = ctx.r7.u32;
        uint32_t chosen = ctx.r9.u32 < count ? ctx.r9.u32 : 0;
        for (uint32_t i = 0; i < count && i < 8; ++i)
            if (wide(active_memory->load<uint32_t>(uint64_t(ctx.r8.u32) + i * 4)).rfind("Sign in", 0) == 0 && chosen == i)
                chosen = (i + 1) % count;
        std::cerr << "NUI_MESSAGE_BOX title=\"" << wide(ctx.r5.u32) << "\" text=\"" << wide(ctx.r6.u32)
                  << "\" buttons=" << ctx.r7.u32 << " result=0x" << std::hex << result << " overlapped=0x" << overlapped
                  << std::dec << " chosen=" << chosen;
        for (uint32_t i = 0; i < ctx.r7.u32 && i < 3; ++i)
            std::cerr << " button" << i << "=\"" << wide(active_memory->load<uint32_t>(uint64_t(ctx.r8.u32) + i * 4)) << '"';
        std::cerr << '\n';
        if (result) active_memory->store<uint32_t>(result, chosen);
        ctx.r3.u64 = overlapped ? complete_overlapped(overlapped, 0) : 0;
        return;
    }
    if (address == 0x82ACB39C && std::string_view(name) == "__imp__XamShowNuiSigninUI" && native_notifications) {
        // The Kinect sign-in panel. There are no profiles yet: like a player
        // closing it, the system UI opens and closes (XN_SYS_UI 1, then 0)
        // and the sign-in state is unchanged. The close waits for the title to
        // have drained the open: the title's pump (82233040) reads the whole
        // queue in one pass, and the gear parts page waits for the "showing"
        // state between the two, so sending both at once hangs it there.
        constexpr uint32_t xn_sys_ui = 0x00000009;
        native_notifications->publish(xn_sys_ui, 1);
        native_notifications->publish_when_drained(xn_sys_ui, 0);
        std::cerr << "NUI_SIGNIN_UI panes=0x" << std::hex << ctx.r3.u32 << " flags=0x" << ctx.r4.u32 << " lr=0x"
                  << ctx.lr << std::dec << " result=0 backend=no-profiles\n";
        ctx.r3.u64 = 0;
        return;
    }
    if (address == 0x82ACBE5C && std::string_view(name) == "__imp__XamShowNuiTroubleshooterUI") {
        // (user index, 0, 0): the Kinect troubleshooter. There is no system
        // UI here; report it shown and closed (0), which completes the
        // title's request (8222CA38).
        static uint32_t shown = 0;
        if (shown++ < 8)
            std::cerr << "NUI_TROUBLESHOOTER user=0x" << std::hex << ctx.r3.u32 << " lr=0x" << ctx.lr << std::dec
                      << " count=" << shown << " result=0 backend=closed\n";
        ctx.r3.u64 = 0;
        return;
    }
    if (address == 0x82ACB43C && std::string_view(name) == "__imp__XamNuiGetDeviceStatus") {
        if (!active_memory) throw RuntimeStop("memory-context", address, "guest memory is not initialized");
        write_nui_device_status(*active_memory, ctx.r3.u32, emulated_nui_device_status);
        std::cerr << "NUI_DEVICE_STATUS output=0x" << std::hex << ctx.r3.u32 << " bytes=24 status=0x"
                  << emulated_nui_device_status << std::dec << " backend=emulated-kinect\n";
        return; // Void ABI; the original wrapper reads status from output + 12.
    }
    if (address == 0x82ACB98C && std::string_view(name) == "__imp__NtFreeVirtualMemory") {
        std::cerr << "ORIGINAL_VIRTUAL_FREE_REQUEST lr=0x" << std::hex << ctx.lr << " base_ptr=0x" << ctx.r3.u32
                  << " size_ptr=0x" << ctx.r4.u32 << " type=0x" << ctx.r5.u32;
        try {
            active_memory->check_write(ctx.r3.u32, 4);
            active_memory->check_write(ctx.r4.u32, 4);
            std::cerr << " base=0x" << active_memory->load<uint32_t>(ctx.r3.u32)
                      << " size=0x" << active_memory->load<uint32_t>(ctx.r4.u32);
        } catch (const RuntimeStop&) { std::cerr << " unreadable=1"; }
        std::cerr << std::dec << '\n';
        if (virtual_memory) {
            const auto base_ptr = ctx.r3.u32, size_ptr = ctx.r4.u32;
            const auto before = virtual_memory->statistics();
            ctx.r3.u64 = virtual_memory->free(base_ptr, size_ptr, ctx.r5.u32, ctx.r6.u32);
            std::cerr << "RESULT NtFreeVirtualMemory status=0x" << std::hex << ctx.r3.u32;
            if (!ctx.r3.u32) {
                const auto after = virtual_memory->statistics();
                std::cerr << " base=0x" << active_memory->load<uint32_t>(base_ptr)
                          << " size=0x" << active_memory->load<uint32_t>(size_ptr)
                          << " reclaimed=0x" << before.committed_bytes - after.committed_bytes
                          << " reserved=0x" << after.reserved_bytes;
            }
            std::cerr << std::dec << '\n';
            return;
        }
    }
    if (address == 0x82ACB63C && std::string_view(name) == "__imp__NtWaitForSingleObjectEx") {
        if (trace_imports) {
            std::cerr << "ORIGINAL_WAIT_REQUEST guest_id=" << current_id << " handle=0x" << std::hex << ctx.r3.u32
                      << " mode=0x" << ctx.r4.u32 << " alertable=0x" << ctx.r5.u32 << " timeout_ptr=0x" << ctx.r6.u32;
            if (ctx.r6.u32) {
                try { std::cerr << " timeout_bits=0x" << active_memory->load<uint64_t>(ctx.r6.u32); }
                catch (const RuntimeStop&) { std::cerr << " timeout_unreadable=1"; }
            }
            std::cerr << std::dec << '\n';
        }
    }
    if (address == 0x82ACB57C && std::string_view(name) == "__imp__RtlInitAnsiString" && active_memory) {
        const uint32_t destination = ctx.r3.u32, source = ctx.r4.u32;
        initialize_ansi_string(*active_memory, destination, source);
        std::cerr << "IMPORT RtlInitAnsiString destination=0x" << std::hex << destination
                  << " source=0x" << source << std::dec
                  << " length=" << active_memory->load<uint16_t>(destination)
                  << " maximum_length=" << active_memory->load<uint16_t>(uint64_t(destination) + 2) << '\n';
        return;
    }
    if (address == 0x82ACC43C && std::string_view(name) == "__imp__RtlInitUnicodeString" && active_memory) {
        initialize_unicode_string(*active_memory, ctx.r3.u32, ctx.r4.u32);
        return;
    }
    if (address == 0x82ACC42C && std::string_view(name) == "__imp__RtlUnicodeStringToAnsiString" && active_memory) {
        ctx.r3.u64 = unicode_string_to_ansi(*active_memory, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32 & 0xFF);
        return;
    }
    if (address == 0x82ACC41C && std::string_view(name) == "__imp__RtlFreeAnsiString" && active_memory) {
        free_ansi_string(*active_memory, ctx.r3.u32);
        return;
    }
    if (address == 0x82ACB6DC && std::string_view(name) == "__imp__MmQueryAddressProtect" &&
        virtual_memory && physical_memory && image_protection) {
        const uint32_t queried_address = ctx.r3.u32;
        uint32_t protection;
        if (VirtualMemory::overlaps_arena(queried_address, 1))
            protection = virtual_memory->query_address_protect(queried_address);
        else if (PhysicalMemory::overlaps_arena(queried_address, 1))
            protection = physical_memory->query_address_protect(queried_address);
        else if (image_protection->contains(queried_address))
            protection = image_protection->query_address_protect(queried_address);
        else
            throw RuntimeStop("memory-protection", queried_address, "unsupported address class for protection query");
        ctx.r3.u64 = protection;
        std::cerr << "RESULT MmQueryAddressProtect address=0x" << std::hex << queried_address
                  << " protection=0x" << protection << " lr=0x" << ctx.lr << std::dec << '\n';
        return;
    }
    if (address == 0x82ACBA0C && std::string_view(name) == "__imp__MmAllocatePhysicalMemoryEx" && physical_memory) {
        std::cerr << "REQUEST MmAllocatePhysicalMemoryEx flags=0x" << std::hex << ctx.r3.u32
                  << " size=0x" << ctx.r4.u32 << " protect=0x" << ctx.r5.u32
                  << " min=0x" << ctx.r6.u32 << " max=0x" << ctx.r7.u32
                  << " alignment=0x" << ctx.r8.u32 << std::dec << '\n';
        const bool write_combined = ctx.r5.u32 == 0x404;
        ctx.r3.u64 = physical_memory->allocate(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                                              ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
        std::cerr << "RESULT MmAllocatePhysicalMemoryEx base=0x" << std::hex << ctx.r3.u32
                  << " committed_bytes=0x" << physical_memory->statistics().committed_bytes << std::dec;
        if (ctx.r3.u32) std::cerr << " cache=" << (write_combined ? "write-combined" : "normal");
        std::cerr << '\n';
        return;
    }
    // MmFreePhysicalMemory(type, address): the D3D runtime releases texture
    // and buffer memory it allocated with MmAllocatePhysicalMemoryEx.
    if (address == 0x82ACBA3C && std::string_view(name) == "__imp__MmFreePhysicalMemory" && physical_memory) {
        const uint32_t freed = ctx.r4.u32;
        const uint32_t freed_size = physical_memory->allocation_size(freed);
        if (!physical_memory->free(freed))
            throw RuntimeStop("physical-memory", freed, "free of an address that is no physical allocation base");
        // The pages may come back as another resource: drop what the renderer
        // cached from them (virtual 0xE0000000 is physical 0x1000).
        if (active_guest_graphics && active_guest_graphics->created() && freed >= 0xE0000000u)
            active_guest_graphics->renderer().invalidate(freed - 0xE0000000u + 0x1000u, freed_size);
        static uint32_t frees = 0;
        if (frees++ < 16)
            std::cerr << "RESULT MmFreePhysicalMemory type=0x" << std::hex << ctx.r3.u32 << " address=0x" << freed
                      << " committed_bytes=0x" << physical_memory->statistics().committed_bytes << std::dec << '\n';
        ctx.r3.u64 = 0;
        return;
    }
    if (address == 0x82ACB23C && std::string_view(name) == "__imp__XGetVideoMode" && active_memory) {
        const auto display = query_native_display_mode();
        VideoMode mode(*active_memory, display);
        mode.query(ctx.r3.u32);
        const auto words = mode.snapshot();
        std::cerr << "IMPORT XGetVideoMode output=0x" << std::hex << ctx.r3.u32 << std::dec
                  << " source=windows-current-display width=" << display.width
                  << " height=" << display.height << " refresh_hz=" << display.refresh_hz
                  << " interlaced=" << words[2] << " widescreen=" << words[3]
                  << " high_definition=" << words[4] << " guest_profile=reference-compatible\n";
        return;
    }
    if (address == 0x82ACC44C && std::string_view(name) == "__imp__MmQueryStatistics" && memory_statistics) {
        const uint32_t output = ctx.r3.u32;
        ctx.r3.u64 = memory_statistics->query(output);
        std::cerr << "IMPORT MmQueryStatistics output=0x" << std::hex << output
                  << " status=0x" << ctx.r3.u32 << std::dec;
        if (!ctx.r3.u32) {
            const auto words = memory_statistics->snapshot();
            std::cerr << " budget_pages=" << words[1] << " kernel_pages=" << words[2]
                      << " available_pages=" << words[3] << " virtual_capacity=" << words[4]
                      << " reserved_virtual=" << words[5] << " stack_pages=" << words[8]
                      << " image_pages=" << words[9] << " virtual_pages=" << words[11];
            std::cerr << " physical_pages=" << words[6];
        }
        std::cerr << '\n';
        return;
    }
    if (const auto status = unavailable_import_status(name, address)) {
        // Explicit native policy: no producer registration, output or events.
        // Preserve guest memory and let the original caller handle failure.
        ctx.r3.u64 = *status;
        std::cerr << "UNAVAILABLE " << name << " address=0x" << std::hex << address
                  << " status=0x" << *status << std::dec << '\n';
        return;
    }
    if (address == 0x82ACBAAC && std::string_view(name) == "__imp__KeQueryPerformanceFrequency" && active_clock) {
        ctx.r3.u64 = GuestClock::frequency;
        std::cerr << "IMPORT KeQueryPerformanceFrequency result=" << ctx.r3.u64 << '\n';
        return;
    }
    if (address == 0x82ACB72C && std::string_view(name) == "__imp__XexGetModuleSection" && executable_module) {
        const auto handle = ctx.r3.u32, resource_name = ctx.r4.u32;
        const auto data_output = ctx.r5.u32, size_output = ctx.r6.u32;
        ctx.r3.u64 = executable_module->get_module_section(handle, resource_name, data_output, size_output);
        std::cerr << "RESULT XexGetModuleSection module=0x" << std::hex << handle
                  << " name_ptr=0x" << resource_name << " status=0x" << ctx.r3.u32;
        if (ctx.r3.u32 == 0) {
            const auto data = active_memory->load<uint32_t>(data_output);
            const auto size = active_memory->load<uint32_t>(size_output);
            active_memory->check(data, size);
            std::cerr << " data=0x" << data << " size=0x" << size
                      << " sha1=" << fingerprint(active_memory->base() + data, size);
        }
        std::cerr << std::dec << '\n';
        return;
    }
    if (address == 0x82ACB73C && std::string_view(name) == "__imp__XexGetModuleHandle" && executable_module) {
        const auto module_name = ctx.r3.u32, output = ctx.r4.u32;
        if (module_name && !native_modules)
            throw RuntimeStop("native-module-request", module_name, "native module namespace is not initialized");
        ctx.r3.u64 = module_name ? native_modules->get_module_handle(module_name, output)
                                : executable_module->get_module_handle(module_name, output);
        std::cerr << "RESULT XexGetModuleHandle name=0x" << std::hex << module_name
                  << " output=0x" << output << " status=0x" << ctx.r3.u32
                  << " module=0x" << active_memory->load<uint32_t>(output)
                  << std::dec << " retained=0\n";
        return;
    }
    if (address == 0x82ACB24C && std::string_view(name) == "__imp__XGetGameRegion" && game_region) {
        ctx.r3.u64 = game_region->query();
        std::cerr << "IMPORT XGetGameRegion source=configured-profile result=0x" << std::hex
                  << ctx.r3.u32 << " lr=0x" << ctx.lr << std::dec << '\n';
        return;
    }
    if (address == 0x82ACB5FC && std::string_view(name) == "__imp__ExGetXConfigSetting" && system_config) {
        const auto category = static_cast<uint16_t>(ctx.r3.u32);
        const auto setting = static_cast<uint16_t>(ctx.r4.u32);
        const auto buffer = ctx.r5.u32;
        const auto capacity = static_cast<uint16_t>(ctx.r6.u32);
        const auto required = ctx.r7.u32;
        std::cerr << "REQUEST ExGetXConfigSetting category=" << category << " setting=" << setting
                  << " buffer=0x" << std::hex << buffer << " capacity=0x" << capacity
                  << " required=0x" << required << " lr=0x" << ctx.lr << std::dec << '\n';
        ctx.r3.u64 = system_config->query(category, setting, buffer, capacity, required);
        std::cerr << "IMPORT ExGetXConfigSetting category=" << category << " setting=" << setting
                  << " buffer=0x" << std::hex << buffer << " capacity=0x" << capacity
                  << " required=0x" << required << " status=0x" << ctx.r3.u32 << std::dec;
        if (setting == 9) std::cerr << " language=" << system_config->language();
        else if (setting == 14) std::cerr << " country=" << system_config->country().value();
        std::cerr << '\n';
        return;
    }
    if (address == 0x82ACB71C && std::string_view(name) == "__imp__XexLoadImage" && native_modules) {
        const uint32_t module_name = ctx.r3.u32, flags = ctx.r4.u32;
        const uint32_t min_version = ctx.r5.u32, output = ctx.r6.u32;
        ctx.r3.u64 = native_modules->load_image(module_name, flags, min_version, output);
        std::cerr << "IMPORT XexLoadImage module=xam.xex flags=0x" << std::hex << flags
                  << " min_version=0x" << min_version << " output=0x" << output
                  << " handle=0x" << NativeModules::xam_handle << std::dec
                  << " load_count=" << native_modules->xam_load_count()
                  << " status=0x" << std::hex << ctx.r3.u32 << std::dec << '\n';
        return;
    }
    if (address == 0x82ACB70C && std::string_view(name) == "__imp__XexGetProcedureAddress" && native_modules) {
        const uint32_t handle = ctx.r3.u32, ordinal = ctx.r4.u32, output = ctx.r5.u32;
        ctx.r3.u64 = native_modules->get_procedure_address(handle, ordinal, output);
        std::cerr << "UNAVAILABLE XexGetProcedureAddress module=xam.xex ordinal=0x" << std::hex << ordinal
                  << " output=0x" << output << " result=0x0 status=0x" << ctx.r3.u32 << std::dec << '\n';
        return;
    }
    if (address == 0x82ACB6FC && std::string_view(name) == "__imp__XexUnloadImage" && native_modules) {
        const uint32_t handle = ctx.r3.u32;
        ctx.r3.u64 = native_modules->unload_image(handle);
        std::cerr << "IMPORT XexUnloadImage handle=0x" << std::hex << handle
                  << " resident=1 load_count=" << std::dec << native_modules->xam_load_count()
                  << " status=0x" << std::hex << ctx.r3.u32 << std::dec << '\n';
        return;
    }
    if (address == 0x82ACB68C && std::string_view(name) == "__imp__RtlNtStatusToDosError") {
        const uint32_t status = ctx.r3.u32;
        auto error = module_status_to_dos_error(status);
        if (status == 0x103) error = 997; // STATUS_PENDING -> ERROR_IO_PENDING; no completion is implied.
#ifdef _WIN32
        if (!error) {
            // Other statuses (e.g. file results): Xbox NTSTATUS values are the
            // Windows ones, so use the host's own table.
            using Convert = unsigned long (__stdcall*)(long);
            static const auto convert = reinterpret_cast<Convert>(
                GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
            if (convert) error = uint32_t(convert(static_cast<long>(status)));
        }
#else
        // The same results for the statuses the file and wait paths return.
        static const std::unordered_map<uint32_t, uint32_t> table{
            {0x00000102, 258},   // STATUS_TIMEOUT -> WAIT_TIMEOUT
            {0x80000005, 234},   // STATUS_BUFFER_OVERFLOW -> ERROR_MORE_DATA
            {0x80000006, 18},    // STATUS_NO_MORE_FILES
            {0xC0000001, 31},    // STATUS_UNSUCCESSFUL -> ERROR_GEN_FAILURE
            {0xC0000002, 1},     // STATUS_NOT_IMPLEMENTED -> ERROR_INVALID_FUNCTION
            {0xC0000008, 6},     // STATUS_INVALID_HANDLE
            {0xC000000D, 87},    // STATUS_INVALID_PARAMETER
            {0xC000000F, 2},     // STATUS_NO_SUCH_FILE -> ERROR_FILE_NOT_FOUND
            {0xC0000011, 38},    // STATUS_END_OF_FILE -> ERROR_HANDLE_EOF
            {0xC0000017, 8},     // STATUS_NO_MEMORY -> ERROR_NOT_ENOUGH_MEMORY
            {0xC0000022, 5},     // STATUS_ACCESS_DENIED
            {0xC0000023, 122},   // STATUS_BUFFER_TOO_SMALL -> ERROR_INSUFFICIENT_BUFFER
            {0xC0000033, 123},   // STATUS_OBJECT_NAME_INVALID -> ERROR_INVALID_NAME
            {0xC0000034, 2},     // STATUS_OBJECT_NAME_NOT_FOUND -> ERROR_FILE_NOT_FOUND
            {0xC0000035, 183},   // STATUS_OBJECT_NAME_COLLISION -> ERROR_ALREADY_EXISTS
            {0xC000003A, 3},     // STATUS_OBJECT_PATH_NOT_FOUND -> ERROR_PATH_NOT_FOUND
            {0xC0000043, 32},    // STATUS_SHARING_VIOLATION
            {0xC000007F, 112},   // STATUS_DISK_FULL
            {0xC00000BA, 5},     // STATUS_FILE_IS_A_DIRECTORY -> ERROR_ACCESS_DENIED
            {0xC0000103, 267},   // STATUS_NOT_A_DIRECTORY -> ERROR_DIRECTORY
        };
        if (!error && status == 0) error = 0;
        if (const auto found = table.find(status); !error && found != table.end()) error = found->second;
#endif
        if (error) {
            ctx.r3.u64 = *error;
            std::cerr << "IMPORT RtlNtStatusToDosError status=0x" << std::hex << status
                      << " result=" << std::dec << *error << '\n';
            return;
        }
    }
    if (address == 0x82ACB94C && std::string_view(name) == "__imp__KeQuerySystemTime" && active_memory) {
        const uint32_t output = ctx.r3.u32;
        query_system_time(*active_memory, output);
        // Void ABI: the service writes memory and preserves the PPC registers.
        std::cerr << "IMPORT KeQuerySystemTime output=0x" << std::hex << output << std::dec;
        if (output) std::cerr << " ticks=" << active_memory->load<uint64_t>(output);
        std::cerr << '\n';
        return;
    }
    const bool tls_alloc = address == 0x82ACBCCC && std::string_view(name) == "__imp__KeTlsAlloc";
    const bool tls_free = address == 0x82ACBCDC && std::string_view(name) == "__imp__KeTlsFree";
    const bool tls_get = address == 0x82ACBCAC && std::string_view(name) == "__imp__KeTlsGetValue";
    const bool tls_set = address == 0x82ACBCBC && std::string_view(name) == "__imp__KeTlsSetValue";
    if ((tls_alloc || tls_free || tls_get || tls_set) && active_memory && thread_local_storage) {
        check_reservation_context(ctx);
        if (active_memory->load<uint32_t>(current_pcr) != current_tls ||
            active_memory->load<uint32_t>(uint64_t(current_thread) + 0x68) != current_tls)
            throw RuntimeStop("thread-context", ctx.r13.u32, "TLS does not match this thread's storage");
        const uint32_t index = ctx.r3.u32, value = ctx.r4.u32;
        if (tls_alloc) ctx.r3.u64 = thread_local_storage->allocate_for(current_tls_dynamic);
        else if (tls_free) ctx.r3.u64 = thread_local_storage->free(index);
        else if (tls_get) ctx.r3.u64 = thread_local_storage->get_for(current_tls_dynamic, index);
        else ctx.r3.u64 = thread_local_storage->set_for(current_tls_dynamic, index, value);
        if (trace_imports) {
            std::cerr << "IMPORT " << std::string_view(name).substr(7) << std::hex;
            if (!tls_alloc) std::cerr << " index=0x" << index;
            if (tls_set) std::cerr << " value=0x" << value;
            std::cerr << " result=0x" << ctx.r3.u32 << std::dec << '\n';
        }
        return;
    }
    if (address == 0x82ACC2DC && std::string_view(name) == "__imp__RtlInitializeCriticalSectionAndSpinCount" && active_memory) {
        const uint32_t section = ctx.r3.u32, spin = ctx.r4.u32;
        ctx.r3.u64 = critical_sections->initialize_and_spin_count(section, spin);
        std::cerr << "IMPORT RtlInitializeCriticalSectionAndSpinCount address=0x" << std::hex << section
                  << " spin=" << std::dec << spin
                  << " encoded=" << unsigned(active_memory->load<uint8_t>(uint64_t(section) + 1))
                  << " status=" << ctx.r3.u32 << '\n';
        return;
    }
    if (address == 0x82ACB5AC && std::string_view(name) == "__imp__XexCheckExecutablePrivilege" && executable_module) {
        const uint32_t privilege = ctx.r3.u32;
        ctx.r3.u64 = executable_module->check_privilege(privilege);
        std::cerr << "IMPORT XexCheckExecutablePrivilege privilege=" << privilege << " result=" << ctx.r3.u32 << '\n';
        return;
    }
    const bool enter_section = address == 0x82ACB4AC && std::string_view(name) == "__imp__RtlEnterCriticalSection";
    const bool leave_section = address == 0x82ACB4BC && std::string_view(name) == "__imp__RtlLeaveCriticalSection";
    if ((enter_section || leave_section) && active_memory) {
        // PCR+0x100 holds the guest thread object pointer; r13 holds the PCR.
        check_reservation_context(ctx);
        const uint32_t section = ctx.r3.u32;
        if (enter_section) {
            auto pending = critical_sections->enter(section, current_thread);
            if (pending) {
                std::cerr << "ORIGINAL_CRITICAL_WAIT_REQUEST guest_id=" << current_id
                          << " address=0x" << std::hex << section
                          << " owner=0x" << active_memory->load<uint32_t>(uint64_t(section) + 24)
                          << " contender=0x" << current_thread << std::dec << '\n';
                bool ready = false;
                if (execution_permit->detached()) {
                    execution_permit->attach();
                    parallel_attached(0);
                }
                execution_permit->run_blocking([&](std::stop_token stop) {
                    ready = pending->wait(stop);
                });
                if (!ready) {
                    if (!execution->stopped())
                        throw RuntimeStop("critical-section-cancelled", section,
                                          "critical-section coordinator stopped outside global shutdown");
                    throw GuestExecutionCancelled();
                }
                critical_sections->complete(*pending);
                std::cerr << "RESULT CriticalSectionWait guest_id=" << current_id
                          << " address=0x" << std::hex << section << std::dec
                          << " acquired=1\n";
            }
        } else critical_sections->leave(section, current_thread);
        // Both imports have a void ABI and preserve the guest register context.
        if (trace_imports) std::cerr << "IMPORT " << (enter_section ? "RtlEnterCriticalSection" : "RtlLeaveCriticalSection")
                  << " address=0x" << std::hex << section
                  << " owner=0x" << active_memory->load<uint32_t>(uint64_t(section) + 24)
                  << " recursion=" << std::dec << active_memory->load<uint32_t>(uint64_t(section) + 20)
                  << " lock_count=0x" << std::hex << active_memory->load<uint32_t>(uint64_t(section) + 16)
                  << std::dec << '\n';
        return;
    }
    if (address == 0x82ACB49C && std::string_view(name) == "__imp__RtlInitializeCriticalSection" && active_memory) {
        const uint32_t section = ctx.r3.u32;
        critical_sections->initialize(section);
        // This import has a void ABI; do not invent a return status in r3.
        std::cerr << "IMPORT RtlInitializeCriticalSection address=0x" << std::hex << section << std::dec << '\n';
        return;
    }
    if (address == 0x82ACB59C && std::string_view(name) == "__imp__KeGetCurrentProcessType") {
        ctx.r3.u64 = user_process_type;
        if (trace_imports) std::cerr << "IMPORT KeGetCurrentProcessType result=0x1\n";
        return;
    }
    if (address == 0x82ACB67C && std::string_view(name) == "__imp__RtlImageXexHeaderField" && executable_module) {
        const uint32_t key = ctx.r4.u32;
        ctx.r3.u64 = executable_module->header_field(ctx.r3.u32, key);
        std::cerr << "IMPORT RtlImageXexHeaderField key=0x" << std::hex << key
                  << " result=0x" << ctx.r3.u32 << std::dec << '\n';
        return;
    }
    if (address == 0x82ACB99C && std::string_view(name) == "__imp__NtAllocateVirtualMemory" && virtual_memory) {
        const uint32_t base_ptr = ctx.r3.u32, size_ptr = ctx.r4.u32;
        const auto trace_word = [](uint32_t pointer) {
            // Observability must not change the service's validation/status.
            try { std::cerr << active_memory->load<uint32_t>(pointer); }
            catch (const RuntimeStop&) { std::cerr << "unreadable"; }
        };
        std::cerr << "REQUEST NtAllocateVirtualMemory lr=0x" << std::hex << ctx.lr << " base_ptr=0x" << base_ptr
                  << " size_ptr=0x" << size_ptr << " base=0x";
        trace_word(base_ptr);
        std::cerr << " size=0x";
        trace_word(size_ptr);
        std::cerr << " type=0x" << ctx.r5.u32 << " protect=0x" << ctx.r6.u32
                  << " debug=0x" << ctx.r7.u32 << std::dec << '\n';
        ctx.r3.u64 = virtual_memory->allocate(base_ptr, size_ptr, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
        std::cerr << "RESULT NtAllocateVirtualMemory status=0x" << std::hex << ctx.r3.u32;
        if (ctx.r3.u32 == 0)
            std::cerr << " base=0x" << active_memory->load<uint32_t>(base_ptr)
                      << " size=0x" << active_memory->load<uint32_t>(size_ptr);
        std::cerr << std::dec << '\n';
        return;
    }
    if (std::string_view(name) == "__imp__KeWaitForSingleObject" && native_sync_objects) {
        // (object, reason, mode, alertable, timeout*): the dispatcher object maps
        // to a native event/semaphore; the wait reuses the handle wait path.
        check_reservation_context(ctx);
        const auto object = ctx.r3.u32;
        const auto handle = dispatcher_handle(object);
        auto prepared = GuestWait::prepare(*active_memory, *native_sync_objects, handle, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
        NativeSyncObjects::WaitResult result{prepared.status, false};
        if (!prepared.status) {
            const auto started = std::chrono::steady_clock::now();
            block_guest([&](std::stop_token stop) { result = prepared.wait(stop); });
            wait_graph_wait(object, started);
            if (result.cancelled) throw GuestExecutionCancelled();
        }
        // An auto-reset event is consumed by the satisfied wait.
        if (!result.status && active_memory->load<uint8_t>(object) == 1) {
            std::lock_guard lock(dispatcher_mutex);
            active_memory->store<uint32_t>(uint64_t(object) + 4, 0);
        }
        ctx.r3.u64 = result.status;
        if (dispatcher_waits++ < 32)
            std::cerr << "RESULT KeWaitForSingleObject guest_id=" << current_id << " object=0x" << std::hex << object
                      << " status=0x" << result.status << std::dec << " infinite=" << prepared.infinite
                      << " backend=windows-sync\n";
        return;
    }
    if (std::string_view(name) == "__imp__KeDelayExecutionThread") {
        // (mode, alertable, interval*): relative intervals only (negative, 100 ns).
        check_reservation_context(ctx);
        const uint32_t mode = ctx.r3.u32, alertable = ctx.r4.u32, interval = ctx.r5.u32;
        if (mode > 1 || alertable || !interval)
            throw RuntimeStop("sync-wait-profile", interval, "unsupported KeDelayExecutionThread profile");
        const auto bits = active_memory->load<uint64_t>(interval);
        if (bits && !(bits >> 63))
            throw RuntimeStop("sync-wait-profile", interval, "absolute delays are not implemented");
        const auto duration = std::chrono::nanoseconds((uint64_t(0) - bits) * 100);
        block_guest([&](std::stop_token stop) {
            // Sleep in slices so a stop request is honoured promptly.
            const auto until = std::chrono::steady_clock::now() + duration;
            if (duration.count() == 0) std::this_thread::yield();
            while (!stop.stop_requested() && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::min<std::chrono::nanoseconds>(until - std::chrono::steady_clock::now(),
                                                                                std::chrono::milliseconds(10)));
        });
        ctx.r3.u64 = 0;
        return;
    }
    if (std::string_view(name) == "__imp__KeWaitForMultipleObjects" && native_sync_objects) {
        // (count, objects*, wait type 0=all/1=any, reason, mode, alertable, timeout*, wait blocks*)
        check_reservation_context(ctx);
        const uint32_t count = ctx.r3.u32, objects = ctx.r4.u32, wait_type = ctx.r5.u32;
        const uint32_t mode = ctx.r7.u32, alertable = ctx.r8.u32, timeout = ctx.r9.u32;
        if (!count || count > 32 || wait_type > 1 || mode > 1 || alertable)
            throw RuntimeStop("sync-wait-profile", count, "unsupported KeWaitForMultipleObjects profile");
        uint32_t milliseconds = 0xFFFFFFFF;
        if (timeout) {
            const auto bits = active_memory->load<uint64_t>(timeout);
            if (bits && !(bits >> 63))
                throw RuntimeStop("sync-wait-profile", timeout, "absolute clock-tracking waits are not implemented");
            const auto magnitude = uint64_t(0) - bits;
            milliseconds = uint32_t((std::min<uint64_t>)(magnitude / 10000 + (magnitude % 10000 != 0), 0xFFFFFFFE));
        }
        std::vector<uint32_t> guest_objects;
        std::vector<std::unique_ptr<NativeSyncObjects::WaitHandle>> retained;
        std::vector<NativeSyncObjects::WaitHandle*> waits;
        for (uint32_t i = 0; i < count; ++i) {
            guest_objects.push_back(active_memory->load<uint32_t>(uint64_t(objects) + i * 4));
            retained.push_back(native_sync_objects->retain_wait(dispatcher_handle(guest_objects.back())));
            if (!retained.back()) throw RuntimeStop("dispatcher-object", guest_objects.back(), "wait target is not waitable");
            waits.push_back(retained.back().get());
        }
        NativeSyncObjects::WaitResult result{};
        const auto started = std::chrono::steady_clock::now();
        block_guest([&](std::stop_token stop) {
            result = NativeSyncObjects::WaitHandle::wait_multiple(waits, wait_type == 0, milliseconds, stop);
        });
        wait_graph_wait(guest_objects[0], started);
        if (result.cancelled) throw GuestExecutionCancelled();
        // Satisfied auto-reset events are consumed.
        std::lock_guard consumed(dispatcher_mutex);
        for (uint32_t i = 0; i < count; ++i)
            if ((wait_type == 0 ? result.status == 0 : result.status == i) && active_memory->load<uint8_t>(guest_objects[i]) == 1)
                active_memory->store<uint32_t>(uint64_t(guest_objects[i]) + 4, 0);
        ctx.r3.u64 = result.status;
        if (dispatcher_waits++ < 32)
            std::cerr << "RESULT KeWaitForMultipleObjects guest_id=" << current_id << " count=" << count
                      << " type=" << (wait_type ? "any" : "all") << " status=0x" << std::hex << result.status
                      << std::dec << " backend=windows-sync\n";
        return;
    }
    if ((std::string_view(name) == "__imp__KeSetEvent" || std::string_view(name) == "__imp__KeResetEvent") &&
        native_sync_objects) {
        // KeSetEvent(event, increment, wait) / KeResetEvent(event): both return
        // the previous signal state, kept in the guest DISPATCHER_HEADER.
        check_reservation_context(ctx);
        const auto object = ctx.r3.u32;
        const auto handle = dispatcher_handle(object);
        const bool set = std::string_view(name) == "__imp__KeSetEvent";
        if (set) wait_graph_signal(object);
        std::lock_guard lock(dispatcher_mutex);
        const auto previous = active_memory->load<uint32_t>(uint64_t(object) + 4);
        const auto status = set ? native_sync_objects->set_event(handle) : native_sync_objects->reset_event(handle);
        if (status) throw RuntimeStop("dispatcher-object", object, "native event update failed");
        active_memory->store<uint32_t>(uint64_t(object) + 4, set ? 1 : 0);
        ctx.r3.u64 = previous;
        return;
    }
    if (std::string_view(name) == "__imp__KeReleaseSemaphore" && native_sync_objects) {
        // KeReleaseSemaphore(semaphore, increment, adjustment, wait) -> previous count.
        check_reservation_context(ctx);
        const auto object = ctx.r3.u32;
        const auto handle = dispatcher_handle(object);
        wait_graph_signal(object);
        std::lock_guard lock(dispatcher_mutex);
        const auto result = native_sync_objects->release_semaphore(handle, ctx.r5.s32);
        if (result.status) throw RuntimeStop("dispatcher-object", object, "native semaphore release failed");
        active_memory->store<uint32_t>(uint64_t(object) + 4, uint32_t(int32_t(result.previous_count) + ctx.r5.s32));
        ctx.r3.u64 = result.previous_count;
        return;
    }
    if (address == 0x82ACB63C && std::string_view(name) == "__imp__NtWaitForSingleObjectEx" && native_sync_objects) {
        check_reservation_context(ctx);
        const auto handle = ctx.r3.u32;
        // A thread handle is signaled when its thread exits.
        auto prepared = GuestWait::prepare(*active_memory, *native_sync_objects,
            handle, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, [](uint32_t other) {
                void* host = guest_threads && GuestThreads::is_handle_range(other) ? guest_threads->host_handle(other) : nullptr;
                return host ? NativeSyncObjects::wait_on_host(host, other) : nullptr;
            });
        NativeSyncObjects::WaitResult result{prepared.status, false};
        if (!prepared.status) {
            const auto started = std::chrono::steady_clock::now();
            block_guest([&](std::stop_token stop) {
                result = prepared.wait(stop);
            });
            wait_graph_wait(handle, started);
            if (result.cancelled) throw GuestExecutionCancelled();
        }
        ctx.r3.u64 = result.status;
        if (trace_imports) std::cerr << "RESULT NtWaitForSingleObjectEx guest_id=" << current_id << " handle=0x" << std::hex << handle
                  << " status=0x" << result.status << std::dec << " milliseconds=" << prepared.milliseconds
                  << " infinite=" << prepared.infinite << " backend=windows-sync\n";
        return;
    }
    if (std::string_view(name) == "__imp__NtWaitForMultipleObjectsEx" && native_sync_objects) {
        // (count, handles*, wait_type 0=WaitAll/1=WaitAny, mode, alertable, timeout*)
        check_reservation_context(ctx);
        const uint32_t count = ctx.r3.u32, handles = ctx.r4.u32, wait_type = ctx.r5.u32;
        if (!count || count > 64 || wait_type > 1)
            throw RuntimeStop("sync-wait-profile", count, "unsupported multiple-object wait");
        active_memory->check(handles, uint64_t(count) * 4);
        std::vector<GuestWait::Prepared> prepared;
        for (uint32_t i = 0; i < count; ++i) {
            prepared.push_back(GuestWait::prepare(*active_memory, *native_sync_objects,
                active_memory->load<uint32_t>(uint64_t(handles) + i * 4), ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
                [](uint32_t other) {
                    void* host = guest_threads && GuestThreads::is_handle_range(other) ? guest_threads->host_handle(other) : nullptr;
                    return host ? NativeSyncObjects::wait_on_host(host, other) : nullptr;
                }));
            if (prepared.back().status) {
                ctx.r3.u64 = prepared.back().status;
                return;
            }
        }
        std::vector<NativeSyncObjects::WaitHandle*> waits;
        for (const auto& item : prepared) waits.push_back(item.target.get());
        const uint32_t timeout = prepared[0].infinite ? 0xFFFFFFFFu
            : uint32_t((std::min<uint64_t>)(prepared[0].milliseconds, 0xFFFFFFFEull));
        NativeSyncObjects::WaitResult result{};
        execution_permit->run_blocking([&](std::stop_token stop) {
            result = NativeSyncObjects::WaitHandle::wait_multiple(waits, wait_type == 0, timeout, stop);
        });
        if (result.cancelled) throw GuestExecutionCancelled();
        ctx.r3.u64 = result.status;
        std::cerr << "RESULT NtWaitForMultipleObjectsEx guest_id=" << current_id << " count=" << count
                  << " wait_all=" << (wait_type == 0) << " status=0x" << std::hex << result.status << std::dec << '\n';
        return;
    }
    if (address == 0x82ACB54C && std::string_view(name) == "__imp__NtResumeThread" && guest_threads) {
        check_reservation_context(ctx);
        const auto handle = ctx.r3.u32, output = ctx.r4.u32;
        // A thread this starts waits for us to block once (thread_resumers).
        {
            std::lock_guard lock(thread_resumers_lock);
            thread_resumers[handle] = {current_id, GuestExecution::blocking_waits(current_id)};
        }
        const auto result = guest_threads->resume(handle, output);
        if (result.status || !result.started) {
            std::lock_guard lock(thread_resumers_lock);
            thread_resumers.erase(handle);
        }
        ctx.r3.u64 = result.status;
        if (trace_imports) std::cerr << "RESULT NtResumeThread handle=0x" << std::hex << handle << " output=0x" << output
                  << " status=0x" << result.status << std::dec << " previous=" << result.previous
                  << " guest_id=" << result.id << " backend=windows-thread\n";
        if (!result.status && result.started) {
            execution->wait_until_ready(result.id);
            execution_permit->renew_quantum();
        }
        return;
    }
    if (address == 0x82ACB51C && std::string_view(name) == "__imp__NtSuspendThread" && guest_threads) {
        check_reservation_context(ctx);
        const uint32_t own = guest_threads->handle_for_object(current_thread);
        const auto handle = ctx.r3.u32 == 0xFFFFFFFE ? own : ctx.r3.u32, output = ctx.r4.u32;
        if (!handle) throw RuntimeStop("thread-suspend", current_id, "the main thread cannot suspend itself");
        const auto result = guest_threads->suspend(handle, output);
        ctx.r3.u64 = result.status;
        if (trace_imports) std::cerr << "RESULT NtSuspendThread handle=0x" << std::hex << handle << " output=0x" << output
                  << " status=0x" << result.status << std::dec << " previous=" << result.previous
                  << " guest_id=" << result.id << " backend=guest-suspend-count\n";
        if (!result.status && handle == own) {
            // A thread that suspends itself stops until another thread resumes it.
            execution_permit->run_blocking([handle](std::stop_token stop) {
                while (!stop.stop_requested() && guest_threads->guest_suspends(handle))
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            });
            if (trace_imports) std::cerr << "RESULT NtSuspendThread resumed handle=0x" << std::hex << handle << std::dec << '\n';
        }
        return;
    }
    if (address == 0x82ACB4FC && std::string_view(name) == "__imp__ObReferenceObjectByHandle" && guest_threads) {
        check_reservation_context(ctx);
        uint32_t handle = ctx.r3.u32;
        const uint32_t type = ctx.r4.u32, output = ctx.r5.u32;
        if (handle == 0xFFFFFFFE) {  // NtCurrentThread()
            const uint32_t own = guest_threads->handle_for_object(current_thread);
            if (!own) {
                // The main thread is not in the registry; its object is permanent.
                if (current_id != 1) throw RuntimeStop("object-profile", current_thread, "current thread has no handle");
                if (type != GuestThreads::object_type) throw RuntimeStop("object-profile", type, "unexpected object type");
                active_memory->check_write(output, 4);
                active_memory->store<uint32_t>(output, current_thread);
                ctx.r3.u64 = 0;
                std::cerr << "RESULT ObReferenceObjectByHandle handle=current object=0x" << std::hex << current_thread
                          << std::dec << " thread=main\n";
                return;
            }
            handle = own;
        }
        if (!GuestThreads::is_handle_range(handle))
            throw RuntimeStop("object-profile", handle, "object reference supports owned thread handles only");
        ctx.r3.u64 = guest_threads->reference(handle, type, output);
        std::cerr << "RESULT ObReferenceObjectByHandle handle=0x" << std::hex << handle
                  << " type=0x" << type << " output=0x" << output << " status=0x" << ctx.r3.u32;
        if (!ctx.r3.u32) {
            const auto object = guest_threads->snapshot(handle).state.thread_object;
            std::cerr << " object=0x" << object << std::dec << " references=" << guest_threads->references(object);
        }
        std::cerr << std::dec << '\n';
        return;
    }
    if (address == 0x82ACB4CC && std::string_view(name) == "__imp__ObDereferenceObject" && guest_threads) {
        check_reservation_context(ctx);
        const auto object = ctx.r3.u32;
        if (object == diagnostic_thread && !guest_threads->owns_object(object)) {
            ctx.r3.u64 = 0;  // permanent main thread object
            return;
        }
        guest_threads->dereference(object);
        ctx.r3.u64 = 0;
        std::cerr << "RESULT ObDereferenceObject object=0x" << std::hex << object << std::dec
                  << " references=" << guest_threads->references(object) << '\n';
        return;
    }
    if (std::string_view(name) == "__imp__KeQueryBasePriorityThread" && guest_threads) {
        check_reservation_context(ctx);
        const auto object = ctx.r3.u32;
        const int32_t priority = object == diagnostic_thread && !guest_threads->owns_object(object)
            ? main_thread_priority : guest_threads->priority(object);
        ctx.r3.u64 = uint64_t(int64_t(priority));
        std::cerr << "RESULT KeQueryBasePriorityThread object=0x" << std::hex << object << std::dec
                  << " priority=" << priority << '\n';
        return;
    }
    if (address == 0x82ACB4EC && std::string_view(name) == "__imp__KeSetBasePriorityThread" && guest_threads) {
        check_reservation_context(ctx);
        const auto object = ctx.r3.u32;
        const auto increment = ctx.r4.s32;
        if (object == diagnostic_thread && !guest_threads->owns_object(object)) {
            // The main thread is not in the registry: keep its guest priority.
            ctx.r3.s64 = main_thread_priority;
            std::cerr << "RESULT KeSetBasePriorityThread object=main increment=" << increment
                      << " previous=" << main_thread_priority << " backend=guest-record\n";
            main_thread_priority = increment;
            return;
        }
        const auto previous = guest_threads->set_priority(object, increment);
        ctx.r3.s64 = previous;
        // Saturating increments (15+) make a time-critical thread (e.g. the
        // XAudio mixer the audio callback waits for every 5.33 ms).
        if (const auto id = active_memory->load<uint32_t>(uint64_t(object) + 0x14c))
            execution->set_urgent(id, increment >= 15);
        std::cerr << "RESULT KeSetBasePriorityThread object=0x" << std::hex << object << std::dec
                  << " increment=" << increment << " previous=" << previous
                  << " current=" << guest_threads->priority(object) << " backend=windows-thread\n";
        return;
    }
    if (address == 0x82ACB53C && std::string_view(name) == "__imp__KeSetAffinityThread" && guest_threads) {
        check_reservation_context(ctx);
        const auto object = ctx.r3.u32, mask = ctx.r4.u32, output = ctx.r5.u32;
        ctx.r3.u64 = guest_threads->set_affinity(object, mask, output);
        std::cerr << "RESULT KeSetAffinityThread object=0x" << std::hex << object
                  << " mask=0x" << mask << " status=0x" << ctx.r3.u32;
        if (!ctx.r3.u32) {
            if (output) std::cerr << " previous=0x" << active_memory->load<uint32_t>(output);
            std::cerr << " host_mask=0x" << guest_threads->host_affinity(object);
        }
        std::cerr << std::dec << " backend=windows-thread\n";
        return;
    }
    if (address == 0x82ACB77C && std::string_view(name) == "__imp__ExCreateThread" && guest_threads) {
        check_reservation_context(ctx);
        // A thread created running is created suspended and resumed at once
        // through the same path as NtResumeThread.
        const bool start_running = (ctx.r9.u32 & 0x00FFFFFFu) == 0;
        const GuestThreads::Request request{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
            ctx.r7.u32, ctx.r8.u32, start_running ? (ctx.r9.u32 | 1) : ctx.r9.u32,
            active_memory->load<uint8_t>(uint64_t(ctx.r13.u32) + 0x10C)};
        std::cerr << "REQUEST ExCreateThread output=0x" << std::hex << request.handle_output
                  << " stack_size=0x" << ctx.r4.u32 << " id_output=0x" << ctx.r5.u32
                  << " startup=0x" << ctx.r6.u32 << " worker=0x" << ctx.r7.u32
                  << " argument=0x" << ctx.r8.u32 << " flags=0x" << ctx.r9.u32 << std::dec << '\n';
        ctx.r3.u64 = guest_threads->create(request);
        const auto snapshot = guest_threads->snapshot(active_memory->load<uint32_t>(request.handle_output));
        const auto& state = snapshot.state;
        std::cerr << "RESULT ExCreateThread output=0x" << std::hex << request.handle_output
                  << " handle=0x" << state.handle << " status=0x" << ctx.r3.u32
                  << " startup=0x" << state.startup << " worker=0x" << state.worker
                  << " argument=0x" << state.argument << " flags=0x" << request.flags
                  << " pcr=0x" << state.pcr << " thread=0x" << state.thread_object
                  << " tls=0x" << state.tls_static << " stack_limit=0x" << state.stack_limit
                  << " stack_base=0x" << state.stack_base << std::dec << " guest_id=" << state.id
                  << " native_id=" << snapshot.native_id << " suspended=" << snapshot.suspended
                  << " entry_started=" << snapshot.entry_started << " backend=windows-thread\n";
        std::cerr << "THREAD_CREATION_PROCESSOR guest_id=" << state.id << " parent_cpu=" << request.parent_cpu
                  << " pcr_cpu=" << unsigned(active_memory->load<uint8_t>(uint64_t(state.pcr) + 0x10C))
                  << " thread_cpu=" << unsigned(active_memory->load<uint8_t>(uint64_t(state.thread_object) + 0xBF))
                  << " host_mask=0x" << std::hex << guest_threads->host_affinity(state.thread_object)
                  << std::dec << '\n';
        try {
            const auto vtable = active_memory->load<uint32_t>(state.argument);
            const auto target = active_memory->load<uint32_t>(uint64_t(vtable) + 0x24);
            std::cerr << "ORIGINAL_WORKER_TARGET context=0x" << std::hex << state.argument
                      << " vtable=0x" << vtable << " target=0x" << target << std::dec << '\n';
        } catch (const RuntimeStop&) {
            std::cerr << "ORIGINAL_WORKER_TARGET unreadable=1\n";
        }
        if (start_running && !ctx.r3.u32) {
            const auto resumed = guest_threads->resume(state.handle, 0);
            std::cerr << "RESULT ExCreateThread start=running guest_id=" << resumed.id
                      << " status=0x" << std::hex << resumed.status << std::dec << '\n';
            if (!resumed.status && resumed.started) {
                execution->wait_until_ready(resumed.id);
                execution_permit->renew_quantum();
            }
        }
        return;
    }
    if (address == 0x82ACB5EC && std::string_view(name) == "__imp__NtSetEvent" && native_sync_objects) {
        const uint32_t handle = ctx.r3.u32, previous = ctx.r4.u32;
        if (previous)
            throw RuntimeStop("event-previous-state", previous, "atomic previous event state output is unimplemented");
        wait_graph_signal(handle);
        ctx.r3.u64 = native_sync_objects->set_event(handle);
        if (trace_imports) std::cerr << "RESULT NtSetEvent guest_id=" << current_id << " handle=0x" << std::hex << handle
                  << " previous_output=0x" << previous << " status=0x" << ctx.r3.u32
                  << " lr=0x" << ctx.lr << std::dec << " backend=windows-event\n";
        return;
    }
    if (address == 0x82ACB6CC && std::string_view(name) == "__imp__NtClearEvent" && native_sync_objects) {
        const uint32_t handle = ctx.r3.u32;
        ctx.r3.u64 = native_sync_objects->reset_event(handle);
        if (trace_imports) std::cerr << "RESULT NtClearEvent handle=0x" << std::hex << handle
                  << " status=0x" << ctx.r3.u32 << std::dec << '\n';
        return;
    }
    if (address == 0x82ACB55C && std::string_view(name) == "__imp__NtCreateEvent" && guest_sync_objects) {
        const uint32_t output = ctx.r3.u32, attributes = ctx.r4.u32, type = ctx.r5.u32, initial = ctx.r6.u32;
        std::cerr << "REQUEST NtCreateEvent output=0x" << std::hex << output
                  << " attributes=0x" << attributes << " type=0x" << type
                  << " initial=0x" << initial << " lr=0x" << ctx.lr << std::dec << '\n';
        ctx.r3.u64 = guest_sync_objects->create_event(output, attributes, type, initial);
        std::cerr << "RESULT NtCreateEvent output=0x" << std::hex << output
                  << " handle=0x" << active_memory->load<uint32_t>(output)
                  << " status=0x" << ctx.r3.u32 << std::dec << " type=" << type
                  << " initial=" << initial << " backend=windows-event";
        if (attributes && !(ctx.r3.u32 & 0x80000000))
            std::cerr << " name=" << native_sync_objects->name(active_memory->load<uint32_t>(output));
        std::cerr << '\n';
        return;
    }
    if (address == 0x82ACB5BC && std::string_view(name) == "__imp__NtCreateSemaphore" && guest_sync_objects) {
        const uint32_t output = ctx.r3.u32, attributes = ctx.r4.u32;
        const int32_t initial = ctx.r5.s32, maximum = ctx.r6.s32;
        std::cerr << "REQUEST NtCreateSemaphore output=0x" << std::hex << output
                  << " attributes=0x" << attributes << " lr=0x" << ctx.lr << std::dec
                  << " initial=" << initial << " maximum=" << maximum << '\n';
        ctx.r3.u64 = guest_sync_objects->create_semaphore(output, attributes, initial, maximum);
        std::cerr << "RESULT NtCreateSemaphore output=0x" << std::hex << output
                  << " handle=0x" << active_memory->load<uint32_t>(output)
                  << " status=0x" << ctx.r3.u32 << std::dec << " initial=" << initial
                  << " maximum=" << maximum << " backend=windows-semaphore";
        if (attributes && !(ctx.r3.u32 & 0x80000000))
            std::cerr << " name=" << native_sync_objects->name(active_memory->load<uint32_t>(output));
        std::cerr << '\n';
        return;
    }
    if (std::string_view(name) == "__imp__XAudioSubmitRenderDriverFrame") {
        // (driver, samples): 256 samples x 6 channels of big-endian floats.
        // No host output yet: the frame is consumed at the hardware rate.
        if (ctx.r3.u32 != 0x41550000) throw RuntimeStop("audio", ctx.r3.u32, "unknown audio render driver");
        active_memory->check(ctx.r4.u32, audio_frame_bytes);
        // SFR_AUDIO=1: play it (off by default, so test runs stay silent).
        static const std::unique_ptr<NativeAudio> output = [] {
            const char* text = std::getenv("SFR_AUDIO");
            if (!text || *text == '0') return std::unique_ptr<NativeAudio>();
            auto created = NativeAudio::create();
            std::cerr << "NATIVE_AUDIO_OUTPUT " << (!created ? "unavailable"
#ifdef _WIN32
                : "xaudio2"
#else
                : "sdl"
#endif
                ) << '\n';
            return created;
        }();
        if (output) output->submit(active_memory->base() + ctx.r4.u32);
        // SFR_AUDIO_DUMP=<file>: the raw frames as submitted (for checking the
        // layout offline).
        static std::ofstream dump = [] {
            const char* path = std::getenv("SFR_AUDIO_DUMP");
            return path ? std::ofstream(path, std::ios::binary) : std::ofstream();
        }();
        if (dump.is_open())
            dump.write(reinterpret_cast<const char*>(active_memory->base() + ctx.r4.u32), audio_frame_bytes);
        if (audio_frames_submitted++ % 1875 == 0)
            std::cerr << "NATIVE_AUDIO_FRAMES submitted=" << audio_frames_submitted
                      << " requested=" << audio_frames_requested << " host_ms="
                      << std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch()).count() << '\n';
        ctx.r3.u64 = 0;
        return;
    }
    if (std::string_view(name) == "__imp__XAudioUnregisterRenderDriverClient") {
        std::cerr << "NATIVE_AUDIO_UNREGISTER driver=0x" << std::hex << ctx.r3.u32 << std::dec << '\n';
        audio_client = {};
        ctx.r3.u64 = 0;
        return;
    }
    if (std::string_view(name) == "__imp__XAudioRegisterRenderDriverClient") {
        // (callback*, driver*): callback* holds {routine, context}. The client is
        // registered, but no audio frames are requested yet (no render thread).
        const uint32_t callback = ctx.r3.u32, driver = ctx.r4.u32;
        active_memory->check(callback, 8);
        active_memory->check_write(driver, 4);
        audio_client = {active_memory->load<uint32_t>(callback), active_memory->load<uint32_t>(uint64_t(callback) + 4)};
        active_memory->store<uint32_t>(driver, 0x41550000);
        ctx.r3.u64 = 0;
        std::cerr << "NATIVE_AUDIO_REGISTER routine=0x" << std::hex << audio_client.routine << " context=0x"
                  << audio_client.context << " driver=0x41550000 lr=0x" << ctx.lr << std::dec << '\n';
        // A system thread (own PCR, TLS and stack) runs the client callback.
        if (!audio_pump_handle && guest_threads) audio_pump_handle = start_system_thread(ctx, system_thread_audio);
        return;
    }
    if (std::string_view(name) == "__imp___snprintf" || std::string_view(name) == "__imp__sprintf" ||
        std::string_view(name) == "__imp___vsnprintf" || std::string_view(name) == "__imp__vsprintf") {
        const std::string_view function(name);
        const bool limited = function == "__imp___snprintf" || function == "__imp___vsnprintf";
        const bool list = function == "__imp___vsnprintf" || function == "__imp__vsprintf";
        const uint32_t buffer = ctx.r3.u32, count = limited ? ctx.r4.u32 : 0;
        const uint32_t format = limited ? ctx.r5.u32 : ctx.r4.u32;
        const uint64_t* gprs[] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64,
                                  &ctx.r7.u64, &ctx.r8.u64, &ctx.r9.u64, &ctx.r10.u64};
        uint32_t index = limited ? 3 : 2;  // first variadic slot
        const uint32_t va_list = list ? (limited ? ctx.r6.u32 : ctx.r5.u32) : 0;
        uint32_t list_index = 0;
        const uint32_t stack = ctx.r1.u32;
        const auto next = [&]() -> uint64_t {
            // va_list: consecutive 8-byte slots. Variadic calls: r3..r10, then the
            // caller's parameter area at sp+0x54 (the layout Xenia uses).
            if (list) return active_memory->load<uint64_t>(uint64_t(va_list) + 8 * list_index++);
            const uint32_t slot = index++;
            return slot < 8 ? *gprs[slot] : active_memory->load<uint64_t>(uint64_t(stack) + 0x54 + (slot - 8) * 8);
        };
        const std::string text = guest_format(*active_memory, format, next);
        ctx.r3.u64 = uint64_t(int64_t(limited ? guest_store_limited(*active_memory, buffer, count, text)
                                              : guest_store(*active_memory, buffer, text)));
        if (printf_calls++ < 16)
            std::cerr << "NATIVE_PRINTF function=" << function.substr(7) << " text=\"" << text << "\" result="
                      << ctx.r3.s32 << '\n';
        return;
    }
    if (std::string_view(name) == "__imp__XAudioGetVoiceCategoryVolume") {
        // (category, float* volume): no dashboard volume change, so full volume.
        active_memory->check_write(ctx.r4.u32, 4);
        active_memory->store<uint32_t>(ctx.r4.u32, 0x3F800000);
        ctx.r3.u64 = 0;
        return;
    }
    if (std::string_view(name) == "__imp__XAudioGetSpeakerConfig") {
        // XAudioGetSpeakerConfig(config*): the configuration Xenia reports (0x00010001).
        active_memory->check_write(ctx.r3.u32, 4);
        active_memory->store<uint32_t>(ctx.r3.u32, 0x00010001);
        ctx.r3.u64 = 0;
        std::cerr << "NATIVE_AUDIO_SPEAKER_CONFIG value=0x00010001 lr=0x" << std::hex << ctx.lr << std::dec << '\n';
        return;
    }
    if (std::string_view(name) == "__imp__ExRegisterTitleTerminateNotification") {
        // (registration*, create): callbacks for kernel-initiated title shutdown.
        // The native host never terminates the title through the kernel, so the
        // routine is recorded and never invoked. Void ABI.
        std::cerr << "NATIVE_TITLE_TERMINATE_NOTIFICATION registration=0x" << std::hex << ctx.r3.u32
                  << " create=" << ctx.r4.u32 << " lr=0x" << ctx.lr << std::dec << " backend=never-signalled\n";
        return;
    }
    if (std::string_view(name) == "__imp__KeEnterCriticalRegion" || std::string_view(name) == "__imp__KeLeaveCriticalRegion") {
        // These only defer kernel APC delivery for the current thread; the native
        // runtime never delivers APCs, so there is nothing to defer. Void ABI.
        if (critical_region_calls++ == 0)
            std::cerr << "NATIVE_CRITICAL_REGION first=" << name << " lr=0x" << std::hex << ctx.lr << std::dec
                      << " backend=no-apc-delivery\n";
        return;
    }
    if (content_files && std::string_view(name).rfind("__imp__XamContent", 0) == 0) {
        // Saves (content_files.h): one hard drive, packages under the save
        // directory, owned by the local profile. The overlapped forms complete
        // at once.
        const std::string_view call(name);
        const auto owner = [](uint32_t user) -> std::optional<uint64_t> {
            if (user == 0xFF || user == 0xFE) return 0;  // XUSER_INDEX_ANY / NONE: device content
            if (user >= 4) return std::nullopt;
            const LocalProfile* profile = profile_for(user);
            return profile ? std::optional<uint64_t>(profile->xuid) : std::nullopt;
        };
        const auto finish = [&](uint32_t result, uint32_t overlapped) {
            ctx.r3.u64 = overlapped ? complete_overlapped(overlapped, result) : result;
        };
        if (call == "__imp__XamContentCreateEx") {
            // (user, root, XCONTENT_DATA*, flags, disposition*, license*, cache size,
            // content size, overlapped at sp+84).
            const uint32_t overlapped = active_memory->load<uint32_t>(uint64_t(ctx.r1.u32) + 84);
            const auto xuid = owner(ctx.r3.u32);
            const uint32_t result = !xuid ? 0x525  // ERROR_NO_SUCH_USER
                : content_files->create(*xuid, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
            std::cerr << "NATIVE_CONTENT_CREATE user=" << ctx.r3.u32 << " flags=0x" << std::hex << ctx.r6.u32
                      << " result=0x" << result << " overlapped=0x" << overlapped << " lr=0x" << ctx.lr << std::dec << '\n';
            finish(result, overlapped);
            return;
        }
        if (call == "__imp__XamContentClose") {
            const uint32_t result = content_files->close(ctx.r3.u32);
            std::cerr << "NATIVE_CONTENT_CLOSE result=0x" << std::hex << result << std::dec << '\n';
            finish(result, ctx.r4.u32);
            return;
        }
        if (call == "__imp__XamContentGetCreator") {
            const auto xuid = owner(ctx.r3.u32);
            finish(!xuid ? 0x525 : content_files->creator(*xuid, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32), ctx.r7.u32);
            return;
        }
        if (call == "__imp__XamContentGetDeviceData") {
            ctx.r3.u64 = content_files->device_data(ctx.r3.u32, ctx.r4.u32);
            return;
        }
        if (call == "__imp__XamContentGetDeviceState") {
            finish(ctx.r3.u32 == ContentFiles::device_id ? 0 : 0x48F, ctx.r4.u32);  // ERROR_DEVICE_NOT_CONNECTED
            return;
        }
    }
    if (content_files && std::string_view(name) == "__imp__XamShowNuiDeviceSelectorUI") {
        // (tracking id?, user, content type, flags, bytes requested, device id*,
        // overlapped): the storage device picker. There is one device, so it
        // is chosen at once, as a player confirming the only choice.
        const uint32_t device_output = ctx.r8.u32, overlapped = ctx.r9.u32;
        if (!device_output) throw RuntimeStop("content-device", 0, "device selector needs a device output");
        active_memory->store<uint32_t>(device_output, ContentFiles::device_id);
        std::cerr << "NUI_DEVICE_SELECTOR user=" << ctx.r4.u32 << " content_type=" << ctx.r5.u32 << " bytes=0x" << std::hex
                  << ctx.r7.u64 << " overlapped=0x" << overlapped << " lr=0x" << ctx.lr << std::dec << " device=1\n";
        ctx.r3.u64 = overlapped ? complete_overlapped(overlapped, 0) : 0;
        return;
    }
    if (address == 0x82ACB62C && std::string_view(name) == "__imp__NtClose" && content_files &&
        content_files->owns(ctx.r3.u32)) {
        const uint32_t handle = ctx.r3.u32;
        ctx.r3.u64 = content_files->close_handle(handle);
        std::cerr << "RESULT NtClose handle=0x" << std::hex << handle << " status=0x" << ctx.r3.u32 << std::dec
                  << " backend=content\n";
        return;
    }
    if (content_files && content_files->owns(ctx.r3.u32) &&
        (std::string_view(name) == "__imp__NtReadFile" || std::string_view(name) == "__imp__NtWriteFile")) {
        // (handle, event, apc, context, io, buffer, length, offset*): synchronous.
        const bool writing = std::string_view(name) == "__imp__NtWriteFile";
        const uint32_t handle = ctx.r3.u32, event = ctx.r4.u32, length = ctx.r9.u32;
        if (ctx.r5.u32) throw RuntimeStop("content-files", ctx.r5.u32, "APC completion is unsupported");
        ctx.r3.u64 = writing ? content_files->write(handle, ctx.r7.u32, ctx.r8.u32, length, ctx.r10.u32)
                             : content_files->read(handle, ctx.r7.u32, ctx.r8.u32, length, ctx.r10.u32);
        if (event) native_sync_objects->set_event(event);
        std::cerr << "RESULT " << (writing ? "NtWriteFile" : "NtReadFile") << " handle=0x" << std::hex << handle
                  << " status=0x" << ctx.r3.u32 << std::dec << " length=" << length << " backend=content\n";
        return;
    }
    if (content_files && content_files->owns(ctx.r3.u32) &&
        (std::string_view(name) == "__imp__NtQueryInformationFile" || std::string_view(name) == "__imp__NtSetInformationFile")) {
        const bool setting = std::string_view(name) == "__imp__NtSetInformationFile";
        const uint32_t handle = ctx.r3.u32, info_class = ctx.r7.u32;
        ctx.r3.u64 = setting ? content_files->set_information(handle, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, info_class)
                             : content_files->query_information(handle, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, info_class);
        std::cerr << "RESULT " << (setting ? "NtSetInformationFile" : "NtQueryInformationFile") << " handle=0x"
                  << std::hex << handle << std::dec << " class=" << info_class << " status=0x" << std::hex
                  << ctx.r3.u32 << std::dec << " backend=content\n";
        return;
    }
    if (std::string_view(name) == "__imp__NtFlushBuffersFile" && content_files && content_files->owns(ctx.r3.u32)) {
        ctx.r3.u64 = content_files->flush(ctx.r3.u32, ctx.r4.u32);
        return;
    }
    if (address == 0x82ACB62C && std::string_view(name) == "__imp__NtClose" && ctx.r3.u32 == 0) {
        // Closing the null handle (e.g. cleanup after a failed open) only reports
        // STATUS_INVALID_HANDLE; no object is involved.
        ctx.r3.u64 = 0xC0000008;
        std::cerr << "RESULT NtClose handle=0x0 status=0xc0000008 lr=0x" << std::hex << ctx.lr << std::dec << '\n';
        return;
    }
    if (address == 0x82ACB62C && std::string_view(name) == "__imp__NtClose" &&
        (guest_files || (native_sync_objects && NativeSyncObjects::is_handle_range(ctx.r3.u32)) ||
         (guest_threads && GuestThreads::is_handle_range(ctx.r3.u32)))) {
        const uint32_t handle=ctx.r3.u32;
        ctx.r3.u64 = native_notifications && native_notifications->owns(handle) ? native_notifications->close(handle) :
            guest_threads && GuestThreads::is_handle_range(handle) ? guest_threads->close(handle) :
            native_sync_objects && NativeSyncObjects::is_handle_range(handle)
            ? native_sync_objects->close(handle) : guest_files->close(handle);
        std::cerr << "RESULT NtClose handle=0x" << std::hex << handle << " status=0x" << ctx.r3.u32 << std::dec << '\n';
        return;
    }
    if (address == 0x82ACB7BC && std::string_view(name) == "__imp__NtReadFile" && active_memory && guest_files) {
        const GuestFiles::ReadRequest request{ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,
            ctx.r7.u32,ctx.r8.u32,ctx.r9.u32,ctx.r10.u32};
        std::cerr << "FILE_READ_ABI guest_id=" << current_id << " lr=0x" << std::hex << ctx.lr
                  << " handle=0x" << request.handle << " event=0x" << request.event
                  << " apc=0x" << request.apc << " context=0x" << request.context
                  << " io=0x" << request.io_output << " buffer=0x" << request.buffer
                  << " length=0x" << request.length << " offset_pointer=0x" << request.offset_pointer
                  << std::dec << '\n';
        const auto result=request.event && guest_async_files
            ? guest_async_files->read(request,*execution_permit) : guest_files->read(request);
        ctx.r3.u64=result.status;
        std::cerr << "RESULT NtReadFile handle=0x" << std::hex << request.handle
                  << " buffer=0x" << request.buffer << std::dec << " requested=" << request.length
                  << " offset=" << result.offset << " transferred=" << result.transferred
                  << " status=0x" << std::hex << result.status << std::dec;
        if (result.transferred)
            std::cerr << " sha1=" << fingerprint(active_memory->base()+request.buffer,result.transferred);
        std::cerr << '\n';
        return;
    }
    if (address == 0x82ACB66C && std::string_view(name) == "__imp__NtQueryInformationFile" && active_memory && guest_files) {
        const uint32_t handle=ctx.r3.u32, io=ctx.r4.u32, output=ctx.r5.u32,
                       length=ctx.r6.u32, info_class=ctx.r7.u32;
        std::cerr << "FILE_INFORMATION_ABI guest_id=" << current_id << " lr=0x" << std::hex << ctx.lr
                  << " handle=0x" << handle << " io=0x" << io << " output=0x" << output
                  << std::dec << " length=" << length << " class=" << info_class << '\n';
        ctx.r3.u64=guest_files->query_information(handle,io,output,length,info_class);
        std::cerr << "RESULT NtQueryInformationFile handle=0x" << std::hex << handle << std::dec
                  << " class=" << info_class << " length=" << length << " status=0x" << std::hex << ctx.r3.u32 << std::dec;
        if (ctx.r3.u32 == 0 && info_class == 34)
            std::cerr << " eof=" << active_memory->load<uint64_t>(uint64_t(output)+40)
                      << " allocation=" << active_memory->load<uint64_t>(uint64_t(output)+32);
        std::cerr << '\n';
        return;
    }
    if (address == 0x82ACB7DC && std::string_view(name) == "__imp__ExTerminateThread") {
        check_reservation_context(ctx);
        if (current_id == 1) throw RuntimeStop("thread-exit", ctx.r3.u32, "the title's main thread terminated itself");
        throw GuestThreadExit{ctx.r3.u32};
    }
    if (std::string_view(name) == "__imp__XamAvatarInitialize") {
        // No avatar system: fail as the pinned reference does (xam_avatar.cc
        // returns ~0). The title's avatar library frees its buffer and continues.
        ctx.r3.u64 = 0xFFFFFFFFu;
        std::cerr << "RESULT XamAvatarInitialize status=0xffffffff backend=no-avatars lr=0x" << std::hex << ctx.lr << std::dec << '\n';
        return;
    }
    if (std::string_view(name) == "__imp__XamAvatarShutdown") {
        std::cerr << "RESULT XamAvatarShutdown backend=no-avatars\n";
        return;
    }
    if (std::string_view(name) == "__imp__XMsgStartIORequestEx" && active_memory) {
        // XMsgStartIORequestEx(app, message, overlapped, buffer, length, ...).
        // Only the music player's XMPSetPlaybackController without an
        // overlapped block: the title keeps playback control (xmp_app.cc).
        const uint32_t app = ctx.r3.u32, message = ctx.r4.u32, overlapped = ctx.r5.u32, buffer = ctx.r6.u32;
        if (app != 0xFA || message != 0x0007001A || overlapped || ctx.r7.u32 != 12)
            throw RuntimeStop("xmsg", message, "unsupported XAM asynchronous message");
        active_memory->check(buffer, 12);
        std::cerr << "RESULT XMPSetPlaybackController client=0x" << std::hex << active_memory->load<uint32_t>(buffer)
                  << " controller=0x" << active_memory->load<uint32_t>(uint64_t(buffer) + 4)
                  << " playback_client=0x" << active_memory->load<uint32_t>(uint64_t(buffer) + 8)
                  << " lr=0x" << ctx.lr << std::dec << '\n';
        ctx.r3.u64 = 0;
        return;
    }
    if (std::string_view(name) == "__imp__XMsgStartIORequest" && active_memory) {
        // XMsgStartIORequest(app, message, overlapped, buffer, length). The
        // XGI app's user context and property writes (rich presence, used by
        // Live only) are accepted and dropped (xgi_app.cc).
        const uint32_t app = ctx.r3.u32, message = ctx.r4.u32, overlapped = ctx.r5.u32;
        // 0x000B0008 is XUserWriteAchievements, sent once a player is signed
        // in (after a race): {count, achievements*} with (user, id) pairs, as
        // Xenia's xgi_app.cc reads it. There is no Live to award them to; the
        // ids are logged where readable, and the request always completes.
        if (app == 0xFB && message == 0x000B0008) {
            const uint32_t buffer = ctx.r6.u32;
            std::cerr << "RESULT XUserWriteAchievements";
            if (active_memory->readable(buffer, 8)) {
                const uint32_t count = active_memory->load<uint32_t>(buffer);
                const uint32_t list = active_memory->load<uint32_t>(uint64_t(buffer) + 4);
                std::cerr << " count=" << count;
                for (uint32_t i = 0; i < count && i < 16 && active_memory->readable(uint64_t(list) + 8 * i, 8); ++i)
                    std::cerr << " id=" << active_memory->load<uint32_t>(uint64_t(list) + 8 * i + 4);
            }
            std::cerr << " backend=no-live\n";
            ctx.r3.u64 = overlapped ? complete_overlapped(overlapped, 0) : 0;
            return;
        }
        if (app != 0xFB || (message != 0x000B0006 && message != 0x000B0007))
            throw RuntimeStop("xmsg", message, "unsupported XAM asynchronous message");
        static uint32_t accepted = 0;
        if (accepted++ < 8)
            std::cerr << "RESULT XMsgStartIORequest app=0x" << std::hex << app << " message=0x" << message
                      << " overlapped=0x" << overlapped << " lr=0x" << ctx.lr << std::dec << " backend=no-live\n";
        ctx.r3.u64 = overlapped ? complete_overlapped(overlapped, 0) : 0;
        return;
    }
    if (address == 0x82ACB42C && std::string_view(name) == "__imp__XMsgInProcessCall" && active_memory) {
        // XMsgInProcessCall(app, message, buffer, length). Only the music
        // player's XMPGetPlaybackController is supported: no dashboard music
        // exists, so the title controls playback (controller 0, unlocked),
        // as in the pinned reference (xam/apps/xmp_app.cc).
        const uint32_t app = ctx.r3.u32, message = ctx.r4.u32, buffer = ctx.r5.u32;
        if (app != 0xFA || message != 0x0007001B)
            throw RuntimeStop("xmsg", message, "unsupported XAM in-process message");
        active_memory->check(buffer, 12);
        const uint32_t client = active_memory->load<uint32_t>(buffer);
        const uint32_t controller = active_memory->load<uint32_t>(uint64_t(buffer) + 4);
        const uint32_t locked = active_memory->load<uint32_t>(uint64_t(buffer) + 8);
        active_memory->check_write(controller, 4);
        active_memory->check_write(locked, 4);
        active_memory->store<uint32_t>(controller, 0);
        active_memory->store<uint32_t>(locked, 0);
        ctx.r3.u64 = 0;
        std::cerr << "RESULT XMPGetPlaybackController client=0x" << std::hex << client << " controller=0 locked=0 lr=0x"
                  << ctx.lr << std::dec << '\n';
        return;
    }
    if (address == 0x82ACB60C && std::string_view(name) == "__imp__NtSetInformationFile" && guest_files) {
        const uint32_t handle = ctx.r3.u32, io = ctx.r4.u32, input = ctx.r5.u32, length = ctx.r6.u32, info_class = ctx.r7.u32;
        ctx.r3.u64 = guest_files->set_information(handle, io, input, length, info_class);
        std::cerr << "RESULT NtSetInformationFile handle=0x" << std::hex << handle << std::dec << " class=" << info_class
                  << " length=" << length << " status=0x" << std::hex << ctx.r3.u32 << std::dec;
        if (info_class == 14 && length >= 8) std::cerr << " position=" << active_memory->load<uint64_t>(input);
        std::cerr << '\n';
        return;
    }
    if (address == 0x82ACB6AC && std::string_view(name) == "__imp__NtQueryFullAttributesFile" && guest_files) {
        const uint32_t attributes = ctx.r3.u32, output = ctx.r4.u32;
        if (content_files) {
            uint32_t data = 0;
            const std::string path = guest_files->object_path(attributes, data);
            if (content_files->claims(path)) {
                ctx.r3.u64 = content_files->query_full_attributes(path, output);
                std::cerr << "RESULT NtQueryFullAttributesFile path=" << path << " status=0x" << std::hex
                          << ctx.r3.u32 << std::dec << " backend=content\n";
                return;
            }
        }
        ctx.r3.u64 = guest_files->query_full_attributes(attributes, output);
        std::cerr << "RESULT NtQueryFullAttributesFile path=" << guest_files->last_path() << " status=0x" << std::hex << ctx.r3.u32;
        if (!ctx.r3.u32) std::cerr << " attributes=0x" << active_memory->load<uint32_t>(uint64_t(output)+48)
                                   << std::dec << " size=" << active_memory->load<uint64_t>(uint64_t(output)+40);
        std::cerr << std::dec << " lr=0x" << std::hex << ctx.lr << std::dec << '\n';
        return;
    }
    if (address == 0x82ACB58C && std::string_view(name) == "__imp__NtCreateFile" && active_memory) {
        std::cerr << "FILE_OPEN_ABI guest_id=" << current_id << " lr=0x" << std::hex << ctx.lr
                  << " sp=0x" << ctx.r1.u32 << " output=0x" << ctx.r3.u32
                  << " access=0x" << ctx.r4.u32 << " attributes=0x" << ctx.r5.u32
                  << " io=0x" << ctx.r6.u32 << " allocation=0x" << ctx.r7.u32
                  << " file_attributes=0x" << ctx.r8.u32 << " share=0x" << ctx.r9.u32
                  << " disposition=0x" << ctx.r10.u32 << std::dec << '\n';
        try {
            const auto descriptor = active_memory->load<uint32_t>(uint64_t(ctx.r5.u32) + 4);
            const auto length = active_memory->load<uint16_t>(descriptor);
            const auto maximum = active_memory->load<uint16_t>(uint64_t(descriptor) + 2);
            const auto data = active_memory->load<uint32_t>(uint64_t(descriptor) + 4);
            if (length && length <= 4096 && maximum >= length && data) {
                active_memory->check(data, length);
                std::string bytes;
                bytes.reserve(size_t(length) * 2);
                constexpr char hex[] = "0123456789abcdef";
                for (uint32_t i = 0; i < length; ++i) {
                    const auto byte = active_memory->load<uint8_t>(uint64_t(data) + i);
                    bytes.push_back(hex[byte >> 4]);
                    bytes.push_back(hex[byte & 15]);
                }
                std::cerr << "FILE_OPEN_NAME data=0x" << std::hex << data << std::dec
                          << " length=" << length << " bytes=" << bytes << '\n';
            }
            if (ctx.lr == 0x824DE908) {
                const auto caller = active_memory->load<uint32_t>(uint64_t(ctx.r1.u32) + 248);
                std::cerr << "FILE_OPEN_CALLER saved_lr=0x" << std::hex << caller;
                if (caller == 0x824DEB24)
                    std::cerr << " outer_lr=0x" << active_memory->load<uint32_t>(uint64_t(ctx.r1.u32) + 360);
                std::cerr << std::dec << '\n';
            }
        } catch (const std::exception&) {
            std::cerr << std::dec << "FILE_OPEN_INSPECTION unavailable\n";
        }
        active_memory->check(ctx.r5.u32, 12);
        std::cerr << "FILE_OPEN_CONTEXT root=0x" << std::hex << active_memory->load<uint32_t>(ctx.r5.u32)
                  << " name=0x" << active_memory->load<uint32_t>(uint64_t(ctx.r5.u32) + 4)
                  << " attributes=0x" << active_memory->load<uint32_t>(uint64_t(ctx.r5.u32) + 8)
                  << " options=0x" << active_memory->load<uint32_t>(uint64_t(ctx.r1.u32) + 84)
                  << std::dec << '\n';
        if (!guest_files)
            throw RuntimeStop("asset-mount", address, "supply an extracted asset directory as the second argument");
        if (content_files) {
            uint32_t data = 0;
            const std::string path = guest_files->object_path(ctx.r5.u32, data);
            if (content_files->claims(path)) {
                const uint32_t handle_output = ctx.r3.u32, disposition = ctx.r10.u32;
                const uint32_t options = active_memory->load<uint32_t>(uint64_t(ctx.r1.u32) + 84);
                ctx.r3.u64 = content_files->open(path, handle_output, ctx.r4.u32, ctx.r6.u32, ctx.r9.u32, disposition, options);
                std::cerr << "RESULT NtCreateFile status=0x" << std::hex << ctx.r3.u32 << " handle=0x"
                          << (ctx.r3.u32 ? 0 : active_memory->load<uint32_t>(handle_output)) << std::dec
                          << " path=" << path << " host=" << content_files->host_path(path).string()
                          << " disposition=" << disposition << " options=0x" << std::hex << options << std::dec
                          << " backend=content\n";
                return;
            }
        }
        const GuestFiles::OpenRequest request{ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,
            ctx.r7.u32,ctx.r8.u32,ctx.r9.u32,ctx.r10.u32,
            active_memory->load<uint32_t>(uint64_t(ctx.r1.u32)+84)};
        ctx.r3.u64 = guest_files->open(request);
        const auto handle = active_memory->load<uint32_t>(request.handle_output);
        std::cerr << "RESULT NtCreateFile status=0x" << std::hex << ctx.r3.u32 << " handle=0x" << handle
                  << std::dec << " size=" << (ctx.r3.u32 == 0 ? asset_files->size(handle) : 0)
                  << " path=" << guest_files->last_path() << '\n';
        if (ctx.r3.u32 == 0) {
            const auto information = asset_files->native_information(handle);
            std::cerr << "NATIVE_FILE_MODE handle=0x" << std::hex << handle
                      << " access=0x" << information.access_flags << " mode=0x" << information.mode
                      << " alignment_requirement=0x" << information.alignment_requirement
                      << std::dec << " backend=windows-file\n";
        }
        return;
    }
    // Capture raw ABI registers without guessing argument types or dereferencing
    // guest pointers. The existing stop and all guest state remain unchanged.
    std::cerr << "IMPORT_CONTEXT name=" << name << " address=0x" << std::hex << address
              << " lr=0x" << ctx.lr << " sp=0x" << ctx.r1.u64
              << " r3=0x" << ctx.r3.u64 << " r4=0x" << ctx.r4.u64
              << " r5=0x" << ctx.r5.u64 << " r6=0x" << ctx.r6.u64
              << " r7=0x" << ctx.r7.u64 << " r8=0x" << ctx.r8.u64
              << " r9=0x" << ctx.r9.u64 << " r10=0x" << ctx.r10.u64 << std::dec << '\n';
    throw RuntimeStop("import-function", address, name);
}
static void trace_original_declarations() {
    // The original caller has stored all four creator returns before reaching
    // 824BBCE8. Inspect real guest metadata; never create or repair a declaration.
    constexpr uint32_t sources[] = {0x821A94D4, 0x821AA938, 0x821AA968, 0x821A94F8};
    constexpr uint32_t counts[] = {2, 3, 3, 4};
    for (uint32_t index = 0; index < 4; ++index) {
        try {
            const uint32_t pointer = active_memory->load<uint32_t>(0x83E50A90 + index * 4);
            const uint64_t object = pointer;
            active_memory->check(object, 56 + 12 * counts[index]);
            const auto word = [&](uint32_t offset) { return active_memory->load<uint32_t>(object + offset); };
            bool metadata = pointer != 0 && word(0) == 0x00100005 && word(4) == 1 &&
                word(20) == 0xFFFF0000 && word(24) == counts[index] && word(28) == 0 && word(48) == 0;
            for (uint32_t byte = 0; byte < 16; ++byte)
                metadata &= active_memory->load<uint8_t>(object + 32 + byte) == (byte == 0 ? 0xFF : 0);
            bool elements_match = true;
            for (uint32_t byte = 0; byte < 12 * counts[index]; ++byte)
                elements_match &= active_memory->load<uint8_t>(object + 52 + byte) ==
                                  active_memory->load<uint8_t>(uint64_t(sources[index]) + byte);
            std::cerr << "ORIGINAL_VERTEX_DECLARATION index=" << index << " pointer=0x" << std::hex << pointer
                      << std::dec << " count=" << word(24) << " metadata_valid=" << metadata
                      << " elements_match=" << elements_match << '\n';
        } catch (const RuntimeStop&) {
            std::cerr << "ORIGINAL_VERTEX_DECLARATION index=" << std::dec << index << " inspection=unavailable\n";
        }
    }
}

static void trace_original_renderer_declarations() {
    constexpr uint32_t source_base = 0x820D2BF8;
    constexpr uint32_t global_base = 0x82B62938;
    constexpr uint32_t counts[] = {1, 2, 3, 1, 2, 3, 2, 3, 2, 3, 4, 4};
    for (uint32_t index = 0; index < std::size(counts); ++index) {
        const uint32_t source = source_base + index * 60;
        const uint32_t global = global_base + index * 4;
        try {
            const uint32_t count = counts[index];
            active_memory->check(source, uint64_t(count + 1) * 12);
            bool terminator_valid = true;
            for (uint32_t element = 0; element < count; ++element)
                terminator_valid &= active_memory->load<uint16_t>(uint64_t(source) + element * 12) != 0x00FF;
            const uint64_t terminator = uint64_t(source) + count * 12;
            terminator_valid &= active_memory->load<uint32_t>(terminator) == 0x00FF0000 &&
                                active_memory->load<uint32_t>(terminator + 4) == 0xFFFFFFFF &&
                                active_memory->load<uint32_t>(terminator + 8) == 0;

            const uint32_t pointer = active_memory->load<uint32_t>(global);
            if (!pointer)
                throw RuntimeStop("renderer-declaration", global, "null declaration pointer");
            const uint64_t object_size = 56 + uint64_t(count) * 12;
            active_memory->check(pointer, object_size);
            const auto word = [&](uint32_t offset) { return active_memory->load<uint32_t>(uint64_t(pointer) + offset); };
            bool metadata = word(0) == 0x00100005 && word(4) == 1 &&
                word(8) == 0 && word(12) == 0 && word(16) == 0 && word(20) == 0xFFFF0000 &&
                word(24) == count && word(28) == 0 && word(48) == 0;
            for (uint32_t byte = 0; byte < 16; ++byte)
                metadata &= active_memory->load<uint8_t>(uint64_t(pointer) + 32 + byte) ==
                            (byte == 0 ? 0xFF : 0);
            for (uint32_t byte = 0; byte < 4; ++byte)
                metadata &= active_memory->load<uint8_t>(uint64_t(pointer) + 52 + count * 12 + byte) == 0;
            bool elements_match = true;
            for (uint32_t byte = 0; byte < count * 12; ++byte)
                elements_match &= active_memory->load<uint8_t>(uint64_t(pointer) + 52 + byte) ==
                                  active_memory->load<uint8_t>(uint64_t(source) + byte);
            std::cerr << "ORIGINAL_RENDERER_DECLARATION index=" << index << " source=0x" << std::hex
                      << source << " global=0x" << global << " pointer=0x" << pointer << std::dec
                      << " count=" << count << " metadata_valid=" << metadata
                      << " elements_match=" << elements_match << " terminator_valid=" << terminator_valid << '\n';
        } catch (const RuntimeStop&) {
            std::cerr << "ORIGINAL_RENDERER_DECLARATION index=" << std::dec << index
                      << " source=0x" << std::hex << source << " global=0x" << global
                      << std::dec << " inspection=unavailable\n";
        }
    }
}

static void trace_original_renderer_shader_publication(PPCContext& ctx) {
    constexpr uint32_t globals[] = {0x82B62968, 0x82B6296C, 0x82B62970,
                                    0x82B62974, 0x82B62978, 0x82B6297C};
    constexpr uint32_t sources[] = {0x820D2600, 0x820D2728, 0x820D2848,
                                    0x820D2988, 0x820D2A28, 0x820D2B10};
    constexpr uint32_t sizes[] = {296, 284, 320, 160, 232, 232};
    std::array<uint32_t, std::size(globals)> handles{};
    bool owners_valid = active_guest_graphics != nullptr && active_guest_graphics->created();
    bool stages_valid = owners_valid;
    bool sources_match = owners_valid;
    bool unique_handles = owners_valid;
    try {
        for (uint32_t index = 0; index < handles.size(); ++index) {
            handles[index] = active_memory->load<uint32_t>(globals[index]);
            // Globals hold the original shader objects; each maps to one native shader.
            const bool owner_valid = active_guest_graphics &&
                                     active_guest_graphics->shaders().owns_object(handles[index]);
            owners_valid &= owner_valid;
            if (!owner_valid) {
                stages_valid = false;
                sources_match = false;
                continue;
            }
            const auto& shader = active_guest_graphics->shaders().get(
                active_guest_graphics->shaders().handle_of(handles[index]));
            const ShaderStage expected_stage = index < 3 ? ShaderStage::vertex : ShaderStage::pixel;
            stages_valid &= shader.entry && shader.entry->stage == expected_stage;
            active_memory->check(sources[index], sizes[index]);
            bool source_matches = shader.entry && shader.entry->source.size() == sizes[index];
            for (uint32_t byte = 0; source_matches && byte < sizes[index]; ++byte)
                source_matches &= shader.entry->source[byte] ==
                                  active_memory->load<uint8_t>(uint64_t(sources[index]) + byte);
            sources_match &= source_matches;
        }
        for (uint32_t left = 0; left < handles.size(); ++left) {
            unique_handles &= handles[left] != 0;
            for (uint32_t right = left + 1; right < handles.size(); ++right)
                unique_handles &= handles[left] != handles[right];
        }
        std::cerr << "ORIGINAL_RENDERER_SHADER_PUBLICATION result=" << ctx.r3.s32 << " handles=";
        for (uint32_t index = 0; index < handles.size(); ++index)
            std::cerr << (index ? ",0x" : "0x") << std::hex << handles[index];
        std::cerr << std::dec << " owners_valid=" << owners_valid << " stages_valid=" << stages_valid
                  << " sources_match=" << sources_match << " unique_handles=" << unique_handles << '\n';
    } catch (const RuntimeStop&) {
        std::cerr << "ORIGINAL_RENDERER_SHADER_PUBLICATION result=" << ctx.r3.s32
                  << " inspection=unavailable\n";
    }
}
// Original D3D functions audited to run unchanged on the native device: they
// only store Xenos packets through device+48 (discarded by the native 0x824F8720),
// update device shadow fields and dirty flags, and call only functions that
// are native or listed here. The state they shadow reaches the native backend
// only through native consumers; see docs/d3d-packet-writers.md.
static bool original_packet_writer(uint32_t address) {
    switch (address) {
    case 0x824EC0A8: // SetShaderGPRAllocation: +10920 register shadow, packets; no D3D12 equivalent
    case 0x824EB5D0: // marks shader-dependent state dirty (+16/+24/+32), tail-calls 0x824E66B8
    case 0x824E66B8: // screen scissor packets from the +10436/+10440 shadow; writes no fields
    case 0x824EBF20: // stores the vertex declaration at +11992 (0x2ED8), dirty +16 bit 19; a native draw must read it
    // SetVertexShader / SetPixelShader: retire the previous original shader
    // object like SetTexture, store the new one at +12872 / +12868, set dirty
    // bits and copy the shader's embedded constants (object +872 records) into
    // the device constant shadow. The objects are real guest objects built by
    // the original creators; a native draw maps them to native shaders.
    case 0x824EBD08:
    case 0x824EBB00:
    case 0x825E8568: // effect pass apply: reads the pass's VS (+72) and PS (+76) and calls the two setters
    // Original default-state setup after CreateDevice: calls every state setter
    // below with its table default, nulls all 26 textures, then the helpers.
    case 0x82500B38:
    case 0x82505000: // +10916 field and its packet
    case 0x824ED860: // default FVF declaration into +12608/+11992, dirty bit
    case 0x824E8218: // stores +13808
    // Shader float constants (r6 float4 from r5 at register r4 into the
    // shadows the native draw reads: vertex +1920, dirty bits r7 into +0;
    // pixel +6016, dirty +8) and bool constants (vertex +10112, pixel +10128).
    case 0x824EB758:
    case 0x824EB830:
    case 0x824EB908:
    case 0x824EB968:
    // Integer (loop) constants into the +10140 / +10204 shadows. Translated
    // shaders use only the integer constants embedded in the shader, so the
    // native draw does not read these yet.
    case 0x824EB9C8:
    case 0x824EBA20:
    // GetDisplayMode: width/height/format from the presentation parameters the
    // native CreateDevice stores at +0x35BC.., refresh rate from +0x550C.
    case 0x82504FD8:
    // GetStreamSource: stream buffer (+12636), offset from its vertex fetch
    // constant and stride (+12704) as SetStreamSource stored them; AddRef.
    case 0x824E8ED8:
    // SetStreamSource: vertex fetch constant and stride into the shadow, dirty
    // bits, stream pointer +12636. The replaced buffer is retired only when a
    // device fence (+10908) or pending flag (+10912) exists; both stay zero here.
    case 0x824E8DB8:
    // SetIndices: stores the index buffer at +12612, retiring the previous one
    // under the same fence/pending rules as SetStreamSource.
    case 0x824E8F60:
        return true;
    default:
        return false;
    }
}

// The render-state (0x82AD0A60, 101 records) and sampler-state (0x82AD0F20,
// 20 records) default tables name each state's setter (word 1). Those setters
// only update the device shadow and dirty flags; native ones are hooked, the
// rest run their original code. The draw reads the shadow they maintain.
static bool original_state_setter(uint32_t address) {
    static const std::unordered_set<uint32_t> setters = [] {
        std::unordered_set<uint32_t> result;
        for (auto [table, count] : {std::pair{0x82AD0A60u, 101u}, std::pair{0x82AD0F20u, 20u}})
            for (uint32_t i = 0; i < count; ++i)
                result.insert(active_memory->load<uint32_t>(uint64_t(table) + i * 12 + 4));
        return result;
    }();
    return setters.contains(address);
}

// SFR_ALLOW_RENDER_TARGETS=1 (experiment): a race binds its own colour and
// depth surfaces around the scene, which the native renderer aliases to its
// framebuffer. Set here rather than on the first guest function entry: the
// entry diagnostics can be off, and these two flags decide whether a race
// renders at all.
static const bool allow_render_targets = [] {
    const bool allow = std::getenv("SFR_ALLOW_RENDER_TARGETS") != nullptr;
    GuestGraphics::foreign_render_targets = allow;
    sfr::depth_texture_placeholder = allow;
    return allow;
}();

// See diagnostic_hooks.h.
const bool diagnostic_entries = [] {
    if (std::getenv("SFR_SAMPLE_PROFILE") || std::getenv("SFR_HOST_PROFILE")) return true;
    const char* const text = std::getenv("SFR_DIAGNOSTIC_ENTRIES");
    return !text || *text != '0';
}();

// What makes a thread's entries take enter_function_observed.
static bool needs_entry_observation() {
    return parallel_guest || diagnostic_entries || guest_reach || unselected_user.active;
}

void enter_function_observed(PPCContext& ctx, const char* name, uint32_t address) {
    if (parallel_guest) parallel_function_entry(ctx, address);
    guest_checkpoint();
    // Cheap enough to keep either way: a stop still names the function it
    // happened in.
    current_function = name;
    current_address = address;
    if (guest_reach && current_id != 1) {
        static thread_local std::unordered_set<uint32_t> reached;
        if (reached.insert(address).second)
            std::cerr << "GUEST_REACH guest=" << current_id << " function=0x" << std::hex << address << std::dec << '\n';
    }
    // Evidence an audit outside this function requires, and free to
    // collect: both tests fail on their first comparison unless that
    // audit is running.
    if (unselected_user.active && address == 0x822329E0 && ctx.lr == 0x822344F8 &&
        ctx.r3.u32 == unselected_user.record)
        unselected_user.reset_entered = true;
    if (unselected_user.active && unselected_user.converted && address == 0x82A560B4 && ctx.lr == 0x822345EC) {
        if (ctx.r3.u32 != 1)
            throw RuntimeStop("original-user-conversion", ctx.r3.u32, "original NUL conversion wrapper did not return one character");
        std::cerr << "ORIGINAL_USER_NAME_CONVERSION_RETURN index=" << unselected_user.index
                  << " characters=1 lr=0x822345ec\n";
        unselected_user.active = false;
        entry_observed = needs_entry_observation();
    }
    if (!diagnostic_entries) return;
    if (sampler_inline_mip_pending) {
        // The audited MAG bridge has no nested guest calls, so the very next
        // original entry is already past 822116A8's inline mip write and its
        // return, whatever the caller does next. Which function that is
        // belongs to the caller, not to the bridge: it is reported below
        // rather than required to be a particular one.
        const auto state = active_guest_graphics->sampler_filter_state(0);
        const auto dirty = active_memory->load<uint64_t>(GuestGraphics::device_address + 24);
        if (state.magnification != 1 || state.minification != 1 || state.mip != 1 ||
            state.volume_magnification != 1 || state.volume_minification != 1 || !(dirty & 0x80000000))
            throw RuntimeStop("original-sampler-inline", address, "original inline mip/filter result differs from audited request");
        std::cerr << "ORIGINAL_SAMPLER_INLINE_MIP_RETURN source=0x8221176c slot=0 word3=0x" << std::hex
                  << state.word3 << " word4=0x" << state.word4 << " dirty=0x" << dirty
                  << " next=0x" << address << std::dec << " filters=linear/linear/linear\n";
        sampler_inline_mip_pending = false;
    }
    if (address == 0x824E83F0 && ctx.lr == 0x82211760 &&
        ctx.r3.u32 == GuestGraphics::device_address && ctx.r4.u32 == 0 && ctx.r5.u32 == 1)
        sampler_inline_mip_pending = true;
    profile_address.store(uint64_t(current_id) << 32 | address, std::memory_order_relaxed);
#ifdef _WIN32
    static thread_local HANDLE own_thread = [] {
        HANDLE handle = nullptr;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0);
        return handle;
    }();
    profile_host_thread.store(own_thread, std::memory_order_relaxed);
#endif
    // SFR_WATCH_WORD (or a hook setting sfr::watch_word): report each change of
    // that word with the function entered right after it, which names the
    // writer of a field whose owner is not obvious from the sources.
    if (const uint32_t watched = watch_word.load(std::memory_order_relaxed)) {
        static std::atomic<uint32_t> last{~0u};
        // The function entered before this one: when the entry that notices
        // the change is only an epilogue helper, that is the writer.
        static std::atomic<const char*> earlier_name{""};
        static std::atomic<uint32_t> earlier_address{0};
        const uint32_t now = active_memory->load<uint32_t>(watched);
        const uint32_t before = last.exchange(now, std::memory_order_relaxed);
        if (before != now && before != ~0u)
            std::cerr << "WATCH_WORD address=0x" << std::hex << watched << " 0x" << before << " -> 0x" << now
                      << " entering=" << name << "@0x" << address
                      << " after=" << earlier_name.load(std::memory_order_relaxed) << "@0x"
                      << earlier_address.load(std::memory_order_relaxed) << " lr=0x" << ctx.lr << std::dec << '\n';
        earlier_name.store(name, std::memory_order_relaxed);
        earlier_address.store(address, std::memory_order_relaxed);
    }
    trace_entry(address);
    // SFR_DUMP_ENTRY=<hex address>: print r3..r5, LR and the first words at r3
    // for the first entries of that function (debugging aid).
    // SFR_TRACE_ENTRY=<hex>[,<hex>...]: one line per entry of those functions (debugging aid).
    static const std::vector<uint32_t> trace_entries = [] {
        std::vector<uint32_t> result;
        if (const char* t = std::getenv("SFR_TRACE_ENTRY"))
            for (char* end = const_cast<char*>(t); *end;) {
                result.push_back(uint32_t(std::strtoul(end, &end, 16)));
                if (*end == ',') ++end;
                else break;
            }
        return result;
    }();
    // SFR_TRACE_RANGE=<hex>-<hex>: the same for every function in that range on the main thread.
    static const std::pair<uint32_t, uint32_t> trace_range = [] {
        const char* t = std::getenv("SFR_TRACE_RANGE");
        if (!t) return std::pair<uint32_t, uint32_t>{0, 0};
        char* end = nullptr;
        const uint32_t low = uint32_t(std::strtoul(t, &end, 16));
        return std::pair<uint32_t, uint32_t>{low, *end == '-' ? uint32_t(std::strtoul(end + 1, nullptr, 16)) : low};
    }();
    if ((!trace_entries.empty() && std::find(trace_entries.begin(), trace_entries.end(), address) != trace_entries.end()) ||
        (current_id == 1 && address >= trace_range.first && address < trace_range.second))
        std::cerr << "TRACE_ENTRY guest_id=" << current_id << " address=0x" << std::hex << address << " lr=0x" << ctx.lr
                  << " r3=0x" << ctx.r3.u32 << " r4=0x" << ctx.r4.u32 << " r5=0x" << ctx.r5.u32 << " r6=0x" << ctx.r6.u32
                  << std::dec << '\n';
    static const uint32_t dump_entry = [] { const char* t = std::getenv("SFR_DUMP_ENTRY"); return t ? uint32_t(std::strtoul(t, nullptr, 16)) : 0u; }();
    // SFR_DUMP_R3=<hex> restricts the dump to entries with that r3.
    static const uint32_t dump_r3 = [] { const char* t = std::getenv("SFR_DUMP_R3"); return t ? uint32_t(std::strtoul(t, nullptr, 16)) : 0u; }();
    if (dump_entry == address && active_memory && (!dump_r3 || dump_r3 == ctx.r3.u32)) {
        // Once per distinct (r3, LR) pair, at most 64 lines.
        static std::mutex dump_lock;
        static std::set<uint64_t> dumped;
        std::lock_guard dump_guard(dump_lock);
        // SFR_DUMP_EVERY=N dumps every Nth entry instead of each new (r3, LR).
        static const uint32_t dump_every = [] { const char* t = std::getenv("SFR_DUMP_EVERY"); return t ? uint32_t(std::strtoul(t, nullptr, 10)) : 0u; }();
        static uint64_t dump_entries = 0, dump_lines = 0;
        const bool due = dump_every ? dump_entries++ % dump_every == 0 && dump_lines < 256
                                    : dumped.size() < 64 && dumped.insert(uint64_t(ctx.r3.u32) << 32 | uint32_t(ctx.lr)).second;
        if (due) {
            ++dump_lines;
            std::ostringstream text;
            text << "ENTRY_DUMP address=0x" << std::hex << address << " lr=0x" << ctx.lr << " r3=0x" << ctx.r3.u32
                 << " r4=0x" << ctx.r4.u32 << " r5=0x" << ctx.r5.u32 << " r3_words=";
            // SFR_DUMP_OFFSET=<hex> starts the words at r3 + offset.
            static const uint32_t dump_offset = [] { const char* t = std::getenv("SFR_DUMP_OFFSET"); return t ? uint32_t(std::strtoul(t, nullptr, 16)) : 0u; }();
            try { for (int i = 0; i < 16; ++i) text << active_memory->load<uint32_t>(uint64_t(ctx.r3.u32) + dump_offset + i * 4) << ','; }
            catch (...) { text << "unreadable"; }
            std::cerr << text.str() << std::dec << '\n';
        }
    }
    if (stack_dump_requested && active_memory) {
        static thread_local bool dumped = false;
        if (!dumped) {
            dumped = true;
            // Back chain: each frame's first word links to the caller's frame
            // and the saved LR sits 8 bytes below the caller's frame pointer.
            std::ostringstream text;
            text << "GUEST_STACK function=" << name << " address=0x" << std::hex << address << " lr=0x" << ctx.lr << " frames=";
            try {
                uint32_t frame = ctx.r1.u32;
                for (int depth = 0; depth < 24 && frame; ++depth) {
                    const uint32_t caller = active_memory->load<uint32_t>(frame);
                    if (!caller || caller <= frame) break;
                    text << "0x" << active_memory->load<uint32_t>(uint64_t(caller) - 8) << ',';
                    frame = caller;
                }
            } catch (...) { text << "unreadable"; }
            std::cerr << text.str() << std::dec << '\n';
        }
    }
    // SFR_WATCH=<hex address>: report the function entered after each change
    // of the guest word there (a function-granular write watch for debugging).
    static const uint32_t watch = [] { const char* t = std::getenv("SFR_WATCH"); return t ? uint32_t(std::strtoul(t, nullptr, 16)) : 0u; }();
    if (watch && active_memory) {
        static std::atomic<uint32_t> watched{0};
        static std::atomic<uint32_t> previous_function{0};
        uint32_t value = 0;
        try { value = active_memory->load<uint32_t>(watch); } catch (...) {}
        const uint32_t old = watched.exchange(value);
        if (old != value)
            std::cerr << "WATCH address=0x" << std::hex << watch << " old=0x" << old << " new=0x" << value
                      << " before=0x" << address << " previous=0x" << previous_function.load()
                      << " lr=0x" << ctx.lr << std::dec << '\n';
        previous_function = address;
    }
    if (address == 0x824D0B58 && ctx.lr == 0x82439678) {
        std::cerr << "ORIGINAL_COUNTRY_TRANSLATION_RETURN value=" << std::dec << ctx.r3.u32
                  << " next=0x824d0b58 lr=0x82439678\n";
    }
    if (active_memory && address == 0x82213218 && ctx.lr == 0x822118F0) {
        const uint64_t object = uint64_t(ctx.r31.u32) + 1280;
        active_memory->check(object + 168, 13);
        std::cerr << "ORIGINAL_LANGUAGE_SELECTION_RETURN object=0x" << std::hex << object
                  << " next=0x82213218 index=" << std::dec << active_memory->load<uint32_t>(object + 168)
                  << " asset_byte=" << unsigned(active_memory->load<uint8_t>(object + 172))
                  << " japanese=" << active_memory->load<uint32_t>(object + 176)
                  << " fallback_byte=" << unsigned(active_memory->load<uint8_t>(object + 180)) << '\n';
    }
    if (active_memory && address == 0x824ED770 && ctx.lr == 0x827F6494 && ctx.r3.u32 == 0x820D2600)
        trace_original_renderer_declarations();
    if (active_memory && address == 0x82810130 && ctx.lr == 0x827F6408)
        trace_original_renderer_shader_publication(ctx);
    if (address == 0x8250E188 && ctx.lr == 0x8250E9C4) {
        texture_create = {true, ctx.r1.u32, static_cast<uint32_t>(ctx.lr),
                          active_memory->load<uint32_t>(uint64_t(ctx.r1.u32) + 148)};
    }
    if (texture_create.active && address == 0x82A560A8 && ctx.lr == texture_create.caller &&
        uint64_t(ctx.r1.u32) == uint64_t(texture_create.stack) + 224) {
        std::cerr << "ORIGINAL_TEXTURE_CREATE_RETURN result=" << ctx.r3.s32 << " output=0x"
                  << std::hex << texture_create.output << " texture=0x"
                  << active_memory->load<uint32_t>(texture_create.output) << std::dec << '\n';
        texture_create.active = false;
    }
    if (shared_surface_release.resource && address == 0x824E1EE0 &&
        ctx.lr == 0x824DF404 && ctx.r5.u32 == shared_surface_release.resource) {
        shared_surface_release.heap_stack = ctx.r1.u32;
        shared_surface_release.heap_active = true;
        std::cerr << "ORIGINAL_SHARED_SURFACE_HEAP_FREE resource=0x" << std::hex << ctx.r5.u32
                  << " heap=0x" << ctx.r3.u32 << " flags=0x" << ctx.r4.u32
                  << " sp=0x" << ctx.r1.u32 << std::dec << '\n';
    }
    if (shared_surface_release.heap_active && address == 0x82A560B0 && ctx.lr == 0x824E2150 &&
        ctx.r1.u32 == shared_surface_release.heap_stack) {
        std::cerr << "ORIGINAL_SHARED_SURFACE_HEAP_RESULT resource=0x" << std::hex
                  << shared_surface_release.resource << std::dec << " result=" << ctx.r3.u32 << '\n';
        shared_surface_release.heap_active = false;
    }
    if (address == 0x824F19E8) {
        std::cerr << "ORIGINAL_RESOURCE_DESTROY_REQUEST resource=0x" << std::hex << ctx.r3.u32
                  << " lr=0x" << ctx.lr << " sp=0x" << ctx.r1.u32;
        // Diagnostic only: a small resource may end at the heap's committed
        // limit, so these 52 bytes can extend past it. Never stop the original.
        try {
            active_memory->check(ctx.r3.u32, 52);
            std::cerr << " words=";
            for (uint32_t offset = 0; offset < 52; offset += 4)
                std::cerr << (offset ? "," : "") << active_memory->load<uint32_t>(uint64_t(ctx.r3.u32) + offset);
        } catch (const RuntimeStop&) {
            std::cerr << " inspection=unavailable";
        }
        std::cerr << std::dec << '\n';
    }
    if (address == 0x824F1C08) {
        std::cerr << "ORIGINAL_RESOURCE_ACCESS_REQUEST r3=0x" << std::hex << ctx.r3.u32
                  << " r4=0x" << ctx.r4.u64 << " r5=0x" << ctx.r5.u32 << " r6=0x" << ctx.r6.u32
                  << " r7=0x" << ctx.r7.u32 << " r8=0x" << ctx.r8.u32 << " r9=0x" << ctx.r9.u32
                  << " r10=0x" << ctx.r10.u32 << " sp=0x" << ctx.r1.u32 << " lr=0x" << ctx.lr
                  << " option84=0x" << active_memory->load<uint32_t>(uint64_t(ctx.r1.u32) + 84)
                  << " option92=0x" << active_memory->load<uint32_t>(uint64_t(ctx.r1.u32) + 92)
                  << std::dec << '\n';
        std::cerr << "ORIGINAL_RESOURCE_ACCESS_HEADER address=0x" << std::hex << ctx.r3.u32 << " words=";
        for (uint32_t offset = 0; offset < 32; offset += 4)
            std::cerr << (offset ? "," : "") << active_memory->load<uint32_t>(uint64_t(ctx.r3.u32) + offset);
        std::cerr << std::dec << '\n';
    }
    if (address == 0x8250C848 && ctx.lr == 0x8250E6E8) {
        texture_transfer = {true, ctx.r1.u32, ctx.r3.u32};
        std::cerr << "ORIGINAL_TEXTURE_TRANSFER_REQUEST r3=0x" << std::hex << ctx.r3.u32
                  << " r4=0x" << ctx.r4.u32 << " r5=0x" << ctx.r5.u32 << " r6=0x" << ctx.r6.u32
                  << " r7=0x" << ctx.r7.u32 << " r8=0x" << ctx.r8.u32 << " r9=0x" << ctx.r9.u32
                  << " r10=0x" << ctx.r10.u32 << " sp=0x" << ctx.r1.u32 << " lr=0x" << ctx.lr
                  << std::dec << '\n';
    }
    if (texture_transfer.active && address == 0x824F1F50 && ctx.lr == 0x8250E718 &&
        ctx.r1.u32 == texture_transfer.stack && ctx.r3.u32 == texture_transfer.surface) {
        std::cerr << "ORIGINAL_TEXTURE_TRANSFER_RETURN surface=0x" << std::hex << ctx.r3.u32
                  << " caller=0x" << ctx.lr << std::dec << " result=" << ctx.r30.s32 << '\n';
        if (texture_transfer.resource)
            std::cerr << "ORIGINAL_TEXTURE_UNLOCK_STATE resource=0x" << std::hex << texture_transfer.resource
                      << " header=0x" << active_memory->load<uint32_t>(texture_transfer.resource)
                      << std::dec << '\n';
        texture_transfer.active = false;
    }
    if (address == 0x825F8230 && ctx.lr == 0x825F88A0) {
        dds_copy.active = false;
        std::cerr << "ORIGINAL_DDS_BLOCK_COPY destination=0x" << std::hex << ctx.r3.u32
                  << " source=0x" << ctx.r4.u32 << " mode=0x" << ctx.r5.u32
                  << " stride=0x" << ctx.r6.u32 << " count=0x" << ctx.r7.u32
                  << " sp=0x" << ctx.r1.u32 << std::dec << '\n';
        if (ctx.r5.u32 == 0x20001 && ctx.r6.u32 == 2 && ctx.r7.u32 == 0x800) {
            active_memory->check(ctx.r4.u32, dds_copy.source_bytes.size());
            dds_copy.destination = ctx.r3.u32;
            dds_copy.source = ctx.r4.u32;
            dds_copy.stack = ctx.r1.u32;
            for (size_t index = 0; index < dds_copy.source_bytes.size(); ++index)
                dds_copy.source_bytes[index] = active_memory->load<uint8_t>(uint64_t(dds_copy.source) + index);
            dds_copy.active = true;
        }
    }
    // This caller epilogue runs after 825F8230 has returned, not at its own exit.
    if (dds_copy.active && address == 0x82A560A4 && ctx.lr == 0x825F88A0 &&
        uint64_t(ctx.r1.u32) == uint64_t(dds_copy.stack) + 160) {
        active_memory->check(dds_copy.destination, dds_copy.source_bytes.size());
        active_memory->check(dds_copy.source, dds_copy.source_bytes.size());
        bool swapped_pairs = true, source_unchanged = true;
        for (size_t index = 0; index < dds_copy.source_bytes.size(); ++index) {
            swapped_pairs &= active_memory->load<uint8_t>(uint64_t(dds_copy.destination) + index) ==
                             dds_copy.source_bytes[index ^ 1];
            source_unchanged &= active_memory->load<uint8_t>(uint64_t(dds_copy.source) + index) ==
                                dds_copy.source_bytes[index];
        }
        std::cerr << "ORIGINAL_DDS_BLOCK_COPY_RETURN destination=0x" << std::hex << dds_copy.destination
                  << " source=0x" << dds_copy.source << std::dec << " bytes=" << dds_copy.source_bytes.size()
                  << " swapped_pairs=" << swapped_pairs << " source_unchanged=" << source_unchanged
                  << " cache_zeroes=" << cache_block_zeroes << '\n';
        dds_copy.active = false;
    }
    if (address == 0x8253A6A8)
        std::cerr << "ORIGINAL_DDS_PARSER_REQUEST parser=0x" << std::hex << ctx.r3.u32
                  << " data=0x" << ctx.r4.u32 << " size=0x" << ctx.r5.u32
                  << " info_output=0x" << ctx.r6.u32 << " flags=0x" << ctx.r7.u32
                  << " lr=0x" << ctx.lr << std::dec << '\n';
    // The original parser reaches this copy only after its format probe succeeds.
    // r30 still identifies the current parsed image; observe, never supply, its fields.
    if (address == 0x825F8B58 && ctx.lr == 0x8253A980) {
        const uint64_t parser = ctx.r30.u32;
        active_memory->check(parser, 84);
        std::cerr << "ORIGINAL_DDS_COPY_REQUEST parser=0x" << std::hex << parser
                  << " format=0x" << active_memory->load<uint32_t>(parser)
                  << " source=0x" << active_memory->load<uint32_t>(parser + 4)
                  << " destination=0x" << ctx.r3.u32
                  << " row_pitch=0x" << active_memory->load<uint32_t>(parser + 48)
                  << " slice_pitch=0x" << active_memory->load<uint32_t>(parser + 52)
                  << std::dec << " width=" << active_memory->load<uint32_t>(parser + 12)
                  << " height=" << active_memory->load<uint32_t>(parser + 16)
                  << " depth=" << active_memory->load<uint32_t>(parser + 20)
                  << " type=" << active_memory->load<uint32_t>(parser + 68)
                  << " next_mip=" << active_memory->load<uint32_t>(parser + 76)
                  << " next_face=" << active_memory->load<uint32_t>(parser + 80) << '\n';
    }
    if (++thread_calls <= 40 && current_id != 1)
        std::cerr << "WORKER_ENTER guest_id=" << current_id << " name=" << name << " address=0x"
                  << std::hex << address << std::dec << '\n';
    if (++calls > call_budget) throw RuntimeStop("budget", address, "diagnostic call budget exceeded");
    if (active_memory && address == 0x824BBCE8) trace_original_declarations();
    // 0x82211440 is the verified no-argument global getter. Both branches
    // overwrite r3 before using it; a preceding void call may leave a device
    // value in that volatile register without passing the device as an argument.
    // 82A56044 only saves nonvolatile registers/LR to r1-based stack slots; r3 is unused.
    // 82A560BC only restores r29/r30/r31/LR from r1-based slots; all six original
    // instructions are verified and none reads r3 or a native object layout.
    // The original effect loader only saves/passes the device value. 824F44F8
    // solely increments/returns our established BE refcount at device+0x3C;
    // its complete original body has no calls or other resource accesses.
    const bool original_effect_loader = address == 0x825E7F98 && ctx.r3.u32 == GuestGraphics::device_address;
    const bool original_device_addref = address == 0x824F44F8 && ctx.r3.u32 == GuestGraphics::device_address;
    // 824BBCE8 captures the device in r30 and stores it at83E53700 only.
    // Its allocator callback receives the original r4 object, never the device.
    const bool original_device_capture = address == 0x824BBCE8 && ctx.r3.u32 == GuestGraphics::device_address;
    // 824C1048 saves the device value at83E53800; its only other pointer chain
    // uses the initialized original CPU manager, not the native device layout.
    const bool original_renderer_constructor = address == 0x824C1048 && ctx.r3.u32 == GuestGraphics::device_address;
    const bool original_renderer_save = address == 0x82A56054 && ctx.r3.u32 == GuestGraphics::device_address &&
                                       ctx.lr == 0x824C1050;
    // Audited 827F61A0 stores/forwards the device without dereferencing it.
    // Its nested state/resource consumers remain guarded individually.
    const bool original_renderer_initialization = address == 0x827F61A0 && ctx.lr == 0x82224440 &&
        ctx.r3.u32 == GuestGraphics::device_address;
    const bool original_initialization_forwarder = ctx.r3.u32 == GuestGraphics::device_address &&
        ((address == 0x82A56060 && ctx.lr == 0x827F61A8) ||
         (address == 0x82504FA8 && ctx.lr == 0x827F61D0));
    const bool native_blend_control = address == 0x824E9218 &&
        ctx.r3.u32 == GuestGraphics::device_address;
    const bool native_render_state = ctx.r3.u32 == GuestGraphics::device_address &&
        is_native_render_state_entry(address);
    const bool native_texture_binding = address == 0x824F4220 && ctx.r3.u32 == GuestGraphics::device_address;
    const bool native_primitive_restart = address == 0x824E7F68 &&
        ctx.r3.u32 == GuestGraphics::device_address;
    // 0x824F49C0 unbinds all device state: render targets 0..3 (+12616..) and
    // the depth surface (+12632) only when they differ from the defaults
    // (+15044 / +15036), through setters with no native equivalent yet; then
    // shaders, declaration, indices, 16 streams and 26 textures through
    // allowlisted or native setters. It runs unchanged while the targets are
    // the defaults, which the native device never changes.
    bool original_state_reset = false;
    if (address == 0x824F49C0 && ctx.r3.u32 == GuestGraphics::device_address) {
        const uint64_t device = GuestGraphics::device_address;
        // Defaults: RT0 is the back buffer (+15044), RT1..3 are empty (the
        // reset still sets them empty through the allowlisted null path of
        // 0x824E97F8 below) and the depth surface is the default (+15036).
        bool defaults = active_memory->load<uint32_t>(device + 12632) == active_memory->load<uint32_t>(device + 15036) &&
                        active_memory->load<uint32_t>(device + 12616) == active_memory->load<uint32_t>(device + 15044);
        for (uint32_t slot = 1; slot < 4; ++slot)
            defaults &= active_memory->load<uint32_t>(device + 12616 + slot * 4) == 0;
        if (!defaults) {
            std::cerr << "ORIGINAL_STATE_RESET_TARGETS";
            for (uint32_t offset : {12616u, 12620u, 12624u, 12628u, 12632u, 15036u, 15040u, 15044u})
                std::cerr << " +" << offset << "=0x" << std::hex << active_memory->load<uint32_t>(device + offset) << std::dec;
            std::cerr << '\n';
            throw RuntimeStop("native-graphics-function", address, "state reset with a non-default render target");
        }
        original_state_reset = true;
    }
    // SFR_ALLOW_RENDER_TARGETS=1 (experiment): a race binds its own colour and
    // depth surfaces around the scene (GetRenderTarget 824E8FF0,
    // GetDepthStencilSurface 824E9038, SetRenderTarget 824E9E20,
    // SetDepthStencilSurface 824E9B48) and restores the defaults afterwards.
    // The native renderer has no offscreen targets yet and keeps drawing into
    // its own framebuffer, so what the race renders into those surfaces is
    // wrong; this only lets the rest of the frame run for investigation.
    // With it, report each original D3D entry the race reaches once, to show
    // which parts of the interface offscreen targets would need.
    // Under the flag every original D3D entry that takes the native device is
    // let through (and reported once), to see the whole interface a race uses
    // instead of one stop per call.
    if (allow_render_targets && address >= 0x824E0000 && address < 0x82510000 &&
        (ctx.r3.u32 == GuestGraphics::device_address || ctx.r3.u32 == GuestGraphics::color_handle ||
         ctx.r3.u32 == GuestGraphics::depth_handle))
        original_state_reset = true;
    if (allow_render_targets && address >= 0x824E0000 && address < 0x82510000 &&
        ctx.r3.u32 == GuestGraphics::device_address) {
        static std::set<uint32_t> reported;
        if (reported.size() < 200 && reported.insert(address).second)
            std::cerr << "ORIGINAL_D3D_ENTRY address=0x" << std::hex << address << " lr=0x" << ctx.lr
                      << " r4=0x" << ctx.r4.u32 << " r5=0x" << ctx.r5.u32 << " r6=0x" << ctx.r6.u32
                      << std::dec << '\n';
    }
    if (allow_render_targets &&
        ((ctx.r3.u32 == GuestGraphics::device_address &&
          (address == 0x824E8FF0 || address == 0x824E9038 || address == 0x824E9E20 || address == 0x824E9B48 ||
           address == 0x824E97F8 || address == 0x824E9768)) ||
         // AddRef and Release of the returned surface: the atomic increment
         // and decrement at +4 of the native colour/depth handle, which has
         // that word. A release that would destroy the handle is not allowed.
         ((address == 0x824F0E38 || address == 0x824F1F50) &&
          (ctx.r3.u32 == GuestGraphics::color_handle || ctx.r3.u32 == GuestGraphics::depth_handle) &&
          (address == 0x824F0E38 || active_memory->load<uint32_t>(uint64_t(ctx.r3.u32) + 4) > 1))))
        original_state_reset = true;
    // Device::Release (0x824F4800) while other references remain: decrements
    // the count at +60 and returns; only the last release would destroy the
    // device, which the native device does not support.
    if (address == 0x824F4800 && ctx.r3.u32 == GuestGraphics::device_address &&
        active_memory->load<uint32_t>(uint64_t(GuestGraphics::device_address) + 60) > 1)
        original_state_reset = true;
    // SetRenderTarget(1..3, NULL): stores the empty slot, clears that target's
    // colour write mask field in the +10460 shadow and sets a dirty bit; no
    // packets and no other object is read.
    if (address == 0x824E97F8 && ctx.r3.u32 == GuestGraphics::device_address &&
        ctx.r4.u32 >= 1 && ctx.r4.u32 <= 3 && ctx.r5.u32 == 0) {
        original_state_reset = true;
        static uint32_t resets = 0;
        if (resets++ < 4) std::cerr << "ORIGINAL_STATE_RESET lr=0x" << std::hex << ctx.lr << std::dec << '\n';
    }
    if (address == 0x82811050 && ctx.lr == 0x827F63E8) {
        renderer_initialization.float_table_active = true;
        renderer_initialization.float_table_stack = ctx.r1.u32;
    }
    if (renderer_initialization.float_table_active && address == 0x827F8950 &&
        ctx.lr == 0x827F63EC && ctx.r1.u32 == renderer_initialization.float_table_stack) {
        // The original loops write 60 pairs of three-float records. This observes
        // their completed output; it does not supply table values or alter flow.
        active_memory->check(0x83E59580, 1440);
        std::cerr << "ORIGINAL_RENDERER_FLOAT_TABLE_RETURN source=0x82811050 destination=0x83e59580 bytes=1440 sha1="
                  << fingerprint(active_memory->base() + 0x83E59580, 1440) << '\n';
        renderer_initialization.float_table_active = false;
    }
    if (renderer_initialization.active && address == 0x824D1858 && ctx.lr == 0x827F63BC &&
        ctx.r3.u32 == 0x82B628D0 && ctx.r5.u32 == 64 && ctx.r4.u32 == uint64_t(ctx.r1.u32) + 96) {
        active_memory->check(ctx.r4.u32, renderer_initialization.matrix.size());
        std::copy_n(active_memory->base() + ctx.r4.u32, renderer_initialization.matrix.size(),
                    renderer_initialization.matrix.begin());
        renderer_initialization.matrix_active = true;
        renderer_initialization.matrix_source = ctx.r4.u32;
        renderer_initialization.matrix_stack = ctx.r1.u32;
        std::cerr << "ORIGINAL_RENDERER_MATRIX_COPY_REQUEST destination=0x82b628d0 source=0x" << std::hex
                  << ctx.r4.u32 << " sp=0x" << ctx.r1.u32 << std::dec << " bytes=64 sha1="
                  << fingerprint(renderer_initialization.matrix.data(), renderer_initialization.matrix.size()) << '\n';
    }
    if (renderer_initialization.matrix_active && address == 0x8280C350 && ctx.lr == 0x827F63DC &&
        ctx.r1.u32 == renderer_initialization.matrix_stack &&
        ctx.r4.u32 == renderer_initialization.matrix_source && ctx.r3.u32 == 0x82B66770) {
        const auto& matrix = renderer_initialization.matrix;
        active_memory->check(0x82B628D0, matrix.size());
        active_memory->check(renderer_initialization.matrix_source, matrix.size());
        std::cerr << "ORIGINAL_RENDERER_MATRIX_COPY_RETURN destination=0x82b628d0 bytes=64 match="
                  << std::equal(matrix.begin(), matrix.end(), active_memory->base() + 0x82B628D0)
                  << " source_unchanged=" << std::equal(matrix.begin(), matrix.end(),
                       active_memory->base() + renderer_initialization.matrix_source) << '\n';
        renderer_initialization.matrix_active = false;
    }
    if (original_renderer_initialization) {
        check_reservation_context(ctx);
        active_memory->check(ctx.r4.u32, renderer_initialization.parameters.size());
        std::copy_n(active_memory->base() + ctx.r4.u32, renderer_initialization.parameters.size(),
                    renderer_initialization.parameters.begin());
        renderer_initialization.active = true;
        std::cerr << "ORIGINAL_RENDERER_INIT_REQUEST device=0x" << std::hex << ctx.r3.u32
                  << " parameters=0x" << ctx.r4.u32 << std::dec
                  << " vertex_selector=" << ctx.r5.u32 << " pixel_selector=" << ctx.r6.u32 << '\n';
    }
    if (renderer_initialization.active && address == 0x824E9218 && ctx.lr == 0x827F5F68 &&
        ctx.r3.u32 == GuestGraphics::device_address) {
        active_memory->check(0x82004B88, 304);
        active_memory->check(0x83E5A720, 304);
        active_memory->check(0x83E5A8C0, renderer_initialization.parameters.size());
        const bool caps_match = std::equal(active_memory->base() + 0x82004B88,
            active_memory->base() + 0x82004B88 + 304, active_memory->base() + 0x83E5A720);
        const bool parameters_match = std::equal(renderer_initialization.parameters.begin(),
            renderer_initialization.parameters.end(), active_memory->base() + 0x83E5A8C0);
        std::cerr << "ORIGINAL_RENDERER_CPU_SETUP device=0x" << std::hex
                  << active_memory->load<uint32_t>(0x83E5A6E0) << std::dec
                  << " caps_bytes=304 caps_match=" << caps_match
                  << " parameters_bytes=124 parameters_match=" << parameters_match
                  << " width=" << active_memory->load<uint32_t>(0x83E5A8C0)
                  << " height=" << active_memory->load<uint32_t>(0x83E5A8C4)
                  << " vertex_selector=" << active_memory->load<uint32_t>(0x82AF6CD8)
                  << " pixel_selector=" << active_memory->load<uint32_t>(0x82AF6CDC) << '\n';
        renderer_initialization.active = false;
    }
    // These original wrappers only forward the opaque device. They pass the
    // original resource bytes to the original DDS parser before device use.
    const bool original_texture_wrapper = ctx.r3.u32 == GuestGraphics::device_address &&
        (address == 0x8250E960 || address == 0x8250E188);
    const bool original_texture_save = ctx.r3.u32 == GuestGraphics::device_address &&
        ((address == 0x82A56058 && ctx.lr == 0x8250E968) ||
         (address == 0x82A56030 && ctx.lr == 0x8250E190));
    // D6E0 only checks/forwards the device value. C7E8 replaces r3 with 1;
    // 4FA8 replaces it with the output pointer before copying original static caps.
    const bool original_texture_parameters = ctx.r3.u32 == GuestGraphics::device_address &&
        (address == 0x8250D6E0 || address == 0x8250C7E8 || address == 0x82504FA8);
    if (original_texture_parameters && address == 0x8250D6E0) {
        // The E188 caller reaches this point only after its original parser succeeds.
        const uint64_t parser = uint64_t(ctx.r1.u32) + 160;
        active_memory->check(parser, 84);
        std::cerr << "ORIGINAL_DDS_PARSER_STATE parser=0x" << std::hex << parser
                  << " pixels=0x" << active_memory->load<uint32_t>(parser + 4)
                  << " format=0x" << active_memory->load<uint32_t>(parser) << std::dec
                  << " owned=" << active_memory->load<uint32_t>(parser + 56)
                  << " width=" << active_memory->load<uint32_t>(parser + 12)
                  << " height=" << active_memory->load<uint32_t>(parser + 16)
                  << " depth=" << active_memory->load<uint32_t>(parser + 20) << '\n';
    }
    if (original_renderer_constructor) {
        check_reservation_context(ctx);
        const auto flags = active_memory->load<uint32_t>(0x83E52F40);
        const auto manager = active_memory->load<uint32_t>(0x83E52ECC);
        if (!(flags & 1) || manager != 0x83E53BE8)
            throw RuntimeStop("renderer-context", manager, "original CPU manager is not initialized for renderer construction");
        std::cerr << "ORIGINAL_RENDERER_REQUEST device=0x" << std::hex << ctx.r3.u32;
        std::cerr << " singleton_flags=0x" << flags << " manager=0x" << manager
                  << " manager_field_884=0x" << active_memory->load<uint32_t>(uint64_t(manager) + 884)
                  << std::dec << '\n';
    }
    // The caller invokes its original exit registration only after this constructor returns.
    if (address == 0x82A53FE8 && ctx.lr == 0x824994E0) {
        unsigned identity_matrices = 0;
        for (uint32_t matrix = 0; matrix < 3; ++matrix) {
            bool identity = true;
            for (uint32_t row = 0; row < 4; ++row)
                for (uint32_t column = 0; column < 4; ++column)
                    identity &= active_memory->load<uint32_t>(0x83E53740 + matrix * 64 + row * 16 + column * 4) ==
                                (row == column ? 0x3F800000u : 0u);
            identity_matrices += identity;
        }
        std::cerr << "ORIGINAL_RENDERER_STATE object=0x83e53710 device=0x" << std::hex
                  << active_memory->load<uint32_t>(0x83E53800) << " context=0x"
                  << active_memory->load<uint32_t>(0x83E52EEC) << " manager_field=0x"
                  << active_memory->load<uint32_t>(0x83E53804) << std::dec
                  << " identity_matrices=" << identity_matrices << '\n';
    }
    // XenonRecomp names __save*/__rest* only at the configured, signature-verified
    // register save/restore helpers; they touch the stack, never r3's object.
    const std::string_view entered_name(name);
    const bool register_save_restore = entered_name.starts_with("__save") || entered_name.starts_with("__rest");
    // Only the linked D3D runtime (0x824E0000..0x8250FFFF) interprets the device
    // layout. Game code that merely passes the device on is transparent; the
    // runtime functions it reaches are still checked here.
    const bool d3d_runtime = address >= 0x824E0000 && address < 0x82510000;
    if (active_guest_graphics && d3d_runtime && active_guest_graphics->owns_object(ctx.r3.u32) &&
        !register_save_restore &&!original_packet_writer(address) && !original_state_setter(address) && address != 0x824F8720 && address != 0x824E65A0 && address != 0x824F5288 && address != 0x824F52D0 && address != 0x824F56E8 && address != 0x824F8AD8 &&
        address != 0x824F4CF0 && address != 0x824F4858 && address != 0x824FAB08 && address != 0x824F6DC8 && address != 0x824E96C8 &&
        address != 0x824E8C70 && address != 0x824E96B8 && address != 0x82211440 &&
        address != 0x82A56044 && address != 0x82A560BC && !original_renderer_save && !original_effect_loader && !original_device_addref &&
        !original_device_capture && !original_renderer_constructor && !original_texture_wrapper && !original_texture_save &&
        !original_texture_parameters && !original_renderer_initialization && !original_initialization_forwarder &&
        !native_blend_control && !native_render_state && !native_texture_binding && !native_primitive_restart &&
        !original_state_reset) {
        std::cerr << "NATIVE_GRAPHICS_BOUNDARY address=0x" << std::hex << address
                  << " lr=0x" << ctx.lr << " r3=0x" << ctx.r3.u32
                  << " r4=0x" << ctx.r4.u32 << " r5=0x" << ctx.r5.u32
                  << " r6=0x" << ctx.r6.u32 << " r7=0x" << ctx.r7.u32
                  << " r8=0x" << ctx.r8.u32 << " r9=0x" << ctx.r9.u32 << std::dec << '\n';
        throw RuntimeStop("native-graphics-function", address,
                          "original function cannot consume an unimplemented native object layout");
    }
    if (original_effect_loader)
        std::cerr << "ORIGINAL_EFFECT_LOADER source=0x" << std::hex << address
                  << " device=0x" << ctx.r3.u32 << " blob=0x" << ctx.r4.u32 << std::dec << '\n';
    if (calls <= 100) std::cerr << "ENTER " << name << " @0x" << std::hex << address << std::dec << '\n';
    if (active_memory && (address == 0x824ED770 || address == 0x824ED588)) {
        std::cerr << "ORIGINAL_SHADER_CREATE source=0x" << std::hex << address
                  << " stage=" << (address == 0x824ED770 ? "vertex" : "pixel")
                  << " container=0x" << ctx.r3.u32;
        // Diagnostic inspection must not stop an original optional callback
        // which could return before the library reads the shader container.
        try {
            active_memory->check(ctx.r3.u32,12);
            const auto flags=active_memory->load<uint32_t>(ctx.r3.u32);
            const auto virtual_bytes=active_memory->load<uint32_t>(uint64_t(ctx.r3.u32)+4);
            const auto physical_bytes=active_memory->load<uint32_t>(uint64_t(ctx.r3.u32)+8);
            std::cerr << " words=" << flags << ',' << virtual_bytes << ',' << physical_bytes;
            const uint64_t size=uint64_t(virtual_bytes)+physical_bytes;
            if ((flags==0x102A1101 || flags==0x102A1100) && size>=12 && size<=0x100000) {
                active_memory->check(ctx.r3.u32,size);
                std::cerr << std::dec << " bytes=" << size
                          << " sha1=" << fingerprint(active_memory->base()+ctx.r3.u32,size);
            }
        } catch(const RuntimeStop&) { std::cerr << " inspection=unavailable"; }
        std::cerr << std::dec << '\n';
    }
    // Read-only evidence for the Free Riders D3D creation boundary. Record raw
    // registers without assuming a presentation-parameter layout or guest ABI.
    if (address == 0x824F4CF0 && std::string_view(name) == "sub_824F4CF0") {
        std::cerr << "GRAPHICS_ENTRY name=" << name << " address=0x" << std::hex << address
                  << " lr=0x" << ctx.lr << " sp=0x" << ctx.r1.u64
                  << " r3=0x" << ctx.r3.u64 << " r4=0x" << ctx.r4.u64
                  << " r5=0x" << ctx.r5.u64 << " r6=0x" << ctx.r6.u64
                  << " r7=0x" << ctx.r7.u64 << " r8=0x" << ctx.r8.u64 << std::dec << '\n';
        // The verified normal D3D path copies exactly 124 bytes at 0x82500DA0.
        // Mode 2 has null-parameter callers and is deliberately not sampled here.
        if (active_memory && ctx.r4.u32 != 2 && ctx.r7.u32) {
            active_memory->check(ctx.r7.u32, 124);
            std::cerr << "GRAPHICS_PARAMETERS address=0x" << std::hex << ctx.r7.u32 << " words=";
            for (uint32_t offset = 0; offset < 124; offset += 4)
                std::cerr << (offset ? "," : "") << active_memory->load<uint32_t>(uint64_t(ctx.r7.u32) + offset);
            std::cerr << std::dec << '\n';
        }
    }
}
void call_indirect(PPCContext& ctx, uint8_t* base, uint32_t address) {
    const auto found = functions.find(address);
    if (found == functions.end()) throw RuntimeStop("indirect-call", address, "no verified function mapping");
    const bool user_reset = unselected_user.active && unselected_user.reset_entered &&
        current_address == 0x82232598 && ctx.lr == 0x82232A30 && ctx.r3.u32 == unselected_user.record + 32;
    found->second(ctx, base);
    if (user_reset) unselected_user.reset_virtual = address;
}
}

int main(int argc, char** argv) {
    // The trace is written through std::cerr, which is unbuffered: one write
    // per insertion dominated the run time. Buffer it (1 MiB); normal exits
    // and every reported stop flush it. Guest threads still write in order
    // because only the permit owner runs guest code.
    static char trace_buffer[1 << 20];
    std::setvbuf(stderr, trace_buffer, _IOFBF, sizeof trace_buffer);
    std::cerr.unsetf(std::ios::unitbuf);
    struct TraceFlush { ~TraceFlush() { std::cerr.flush(); std::fflush(stderr); } } trace_flush;
    // A hang reports nothing, so the buffer is also written out twice a
    // second: a stuck game's log ends where it stopped (stdio locks the stream).
    std::jthread trace_flusher([](std::stop_token stop) {
        std::mutex mutex;
        std::condition_variable_any wake;
        std::unique_lock lock(mutex);
        while (!stop.stop_requested()) {
            wake.wait_for(lock, stop, std::chrono::milliseconds(500), [] { return false; });
            std::fflush(stderr);
        }
    });
    PPCContext ctx{};
    if (std::getenv("SFR_FUNCTION_TRACE"))
        sfr::entered_functions = std::make_unique<std::atomic<uint32_t>[]>((sfr::trace_limit - sfr::trace_base) / 4 / 32);
    // Runs after every return path below, once guest workers have been joined.
    struct TraceWriter { ~TraceWriter() { sfr::write_function_trace(); } } trace_writer;
    try {
        if (argc < 2 || argc > 4) {
            std::cerr << "Usage: sfr_cpu_diagnostic IMAGE-DUMP-DIRECTORY [EXTRACTED-ASSET-DIRECTORY] [--game-region=ntsc-us]\nThis is a CPU diagnostic, not a playable game.\n";
            return 2;
        }
        std::string_view region_option;
        bool mount_assets = false;
        if (argc >= 3) {
            if (std::string_view(argv[2]).starts_with("--")) {
                if (argc == 4)
                    throw sfr::RuntimeStop("game-region", 0, "duplicate or misplaced launch option");
                region_option = argv[2];
            } else {
                mount_assets = true;
                if (argc == 4) region_option = argv[3];
            }
        }
        if (argc == 4 && region_option.empty())
            throw sfr::RuntimeStop("game-region", 0, "empty game-region option");
        const sfr::GameRegion game_region(region_option);
        sfr::game_region = &game_region;
        if (game_region.configured())
            std::cerr << "NATIVE_GAME_REGION source=command-line profile=ntsc-us value=0x"
                      << std::hex << game_region.query() << std::dec << '\n';
        else std::cerr << "NATIVE_GAME_REGION source=unconfigured\n";
        const std::filesystem::path dump(argv[1]);
        if (!std::filesystem::is_regular_file(dump / "complete.txt"))
            throw std::runtime_error("incomplete image dump");
        if (std::filesystem::file_size(dump / "image.bin") != PPC_IMAGE_SIZE)
            throw std::runtime_error("decoded image size differs from compiled game");
        sfr::GuestClock clock;
        sfr::active_clock = &clock;
        sfr::GuestMemory memory;
        sfr::VirtualMemory allocations(memory);
        sfr::virtual_memory = &allocations;
        sfr::active_memory = &memory;
        memory.map(PPC_IMAGE_BASE, PPC_IMAGE_SIZE);
        std::ifstream image(dump / "image.bin", std::ios::binary);
        if (!image.read(reinterpret_cast<char*>(memory.base() + PPC_IMAGE_BASE), PPC_IMAGE_SIZE))
            throw std::runtime_error("cannot load decoded image");
        if (fingerprint(memory.base() + PPC_IMAGE_BASE, PPC_IMAGE_SIZE) != "bd12a1e7e2807fdda4625626bead89cbdf4d224e")
            throw std::runtime_error("unsupported decoded image fingerprint");
        if (std::filesystem::file_size(dump / "import_variables.tsv") > 16384)
            throw std::runtime_error("unsupported import-table size");
        std::ifstream variables(dump / "import_variables.tsv");
        if (!variables) throw std::runtime_error("missing import-variable table");
        std::string line;
        std::string canonical_table;
        while (std::getline(variables, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            canonical_table += line + '\n';
        }
        if (fingerprint(canonical_table.data(), canonical_table.size()) != "c9f77993cfdc9ad63b4f1103b329d96e595b21e6")
            throw std::runtime_error("unsupported import-table fingerprint");
        if (!std::filesystem::is_regular_file(dump / "xex_header.bin") ||
            std::filesystem::file_size(dump / "xex_header.bin") != 0x5000)
            throw std::runtime_error("missing or unsupported XEX header size; regenerate the image dump");
        std::vector<uint8_t> header(0x5000);
        std::ifstream header_file(dump / "xex_header.bin", std::ios::binary);
        if (!header_file.read(reinterpret_cast<char*>(header.data()), header.size()) ||
            fingerprint(header.data(), header.size()) != "82ef42d3f130b26563fcd85909f2eab613de6c68")
            throw std::runtime_error("unsupported XEX header fingerprint");
        sfr::XexModule module(memory, header);
        sfr::executable_module = &module;
        sfr::ImageProtection image_protection(header, PPC_IMAGE_BASE, PPC_IMAGE_SIZE);
        sfr::image_protection = &image_protection;
        sfr::NativeModules native_modules(memory);
        sfr::native_modules = &native_modules;
        sfr::NativeWinsock native_winsock(memory);
        sfr::native_winsock = &native_winsock;
        sfr::GuestCriticalSections critical_sections(memory);
        sfr::critical_sections = &critical_sections;
        const auto language = sfr::query_native_language();
        const auto country = sfr::query_native_country();
        sfr::SystemConfig system_config(memory, language.xbox_language, country.xbox_country);
        sfr::system_config = &system_config;
        std::cerr << "NATIVE_USER_LANGUAGE source=GetUserDefaultUILanguage windows_langid=0x"
                  << std::hex << language.windows_language_id << std::dec
                  << " xbox_language=" << language.xbox_language << '\n';
        std::cerr << "NATIVE_USER_COUNTRY source=GetUserDefaultGeoName iso=" << country.iso_code
                  << " xbox_country=" << country.xbox_country << '\n';
        sfr::HardwareInfo hardware(memory);
        // XMA decoder (APU) registers at 0x7FEA0000. The audio library locks,
        // kicks contexts and unlocks through them. No XMA decoder is emulated
        // yet: the page accepts the writes and decoding never starts (no sound).
        memory.map(0x7FEA0000, 0x2000);
        std::cerr << "BIND XmaRegisters address=0x7fea0000 size=0x2000 decoder=absent\n";
        sfr::AbsentDebugMonitor debug_monitor(memory);
        sfr::VideoGlobals video_globals(memory);
        sfr::video_globals = &video_globals;
        sfr::TimestampBundle timestamp(memory, [&clock] { return clock.uptime_milliseconds(); });
        std::istringstream validated_variables(canonical_table);
        std::getline(validated_variables, line);
        size_t variable_count = 0;
        while (std::getline(validated_variables, line)) {
            std::istringstream row(line);
            std::string name, address, ordinal;
            if (!std::getline(row, name, '\t') || !std::getline(row, address, '\t') || !std::getline(row, ordinal))
                throw std::runtime_error("invalid import-variable row");
            auto value = std::stoull(address);
            if (value > UINT32_MAX) throw std::runtime_error("invalid variable address");
            if (name == "__imp__XexExecutableModuleHandle" && value == 0x82000778)
                memory.store<uint32_t>(value, sfr::XexModule::handle_address);
            else if (name == "__imp__ExThreadObjectType" && value == 0x820007E8) {
                memory.add_read_only_word(static_cast<uint32_t>(value), [] { return sfr::GuestThreads::object_type; });
                std::cerr << "BIND ExThreadObjectType identity=0x" << std::hex << sfr::GuestThreads::object_type
                          << std::dec << " descriptor=guarded\n";
            }
            else if (name == "__imp__VdGlobalDevice" && value == 0x82000664 && ordinal == "446") {
                memory.add_read_only_word(static_cast<uint32_t>(value), [] {
                    if (!sfr::current_context || !sfr::video_globals)
                        throw sfr::RuntimeStop("video-global", 0x82000664, "video global context is unavailable");
                    const auto& ctx = *sfr::current_context;
                    sfr::check_reservation_context(ctx);
                    if (sfr::current_address != 0x824F19E8 || ctx.lr != 0x824F1A04) {
                        std::cerr << "VIDEO_GLOBAL_UNSUPPORTED_CONTEXT function=0x" << std::hex
                                  << sfr::current_address << " lr=0x" << ctx.lr << std::dec << '\n';
                        throw sfr::RuntimeStop("video-global", 0x82000664, "unaudited original device-global consumer");
                    }
                    const auto caller = sfr::active_memory->load<uint32_t>(uint64_t(ctx.r1.u32) + 104);
                    const auto cell = sfr::video_globals->device_cell_for_shared_surface(
                        ctx.r31.u32, sfr::current_address, static_cast<uint32_t>(ctx.lr), caller);
                    sfr::shared_surface_release = {ctx.r31.u32, 0, false};
                    std::cerr << "VIDEO_GLOBAL_READ cell=0x" << std::hex << cell
                              << " value=0x" << sfr::active_memory->load<uint32_t>(cell)
                              << " resource=0x" << ctx.r31.u32 << " caller=0x" << caller << std::dec << '\n';
                    return cell;
                });
                std::cerr << "BIND VdGlobalDevice cell=0x" << std::hex << sfr::VideoGlobals::device_cell
                          << std::dec << " scope=shared-surface-destruction\n";
            }
            else if (name == "__imp__KeDebugMonitorData" && value == 0x820007D4 && ordinal == "89") {
                memory.add_read_only_word(static_cast<uint32_t>(value), [] { return sfr::AbsentDebugMonitor::address; });
                std::cerr << "BIND KeDebugMonitorData cell=0x" << std::hex << sfr::AbsentDebugMonitor::address
                          << std::dec << " monitor=0x0 profile=absent-xbox-monitor\n";
            }
            else if (name == "__imp__XboxHardwareInfo" && value == 0x82000798) {
                memory.store<uint32_t>(value, sfr::HardwareInfo::address);
                std::cerr << "BIND XboxHardwareInfo address=0x" << std::hex << sfr::HardwareInfo::address
                          << " flags=0x" << memory.load<uint32_t>(sfr::HardwareInfo::address) << std::dec << '\n';
            }
            else if (name == "__imp__XboxKrnlVersion" && value == 0x820005E4) {
                // XBOX_KRNL_VERSION {Major, Minor, Build, Qfe}: the same verified
                // 2.0.12416.0 profile that XamGetSystemVersion reports.
                constexpr uint32_t kernel_version = 0x71210000;
                memory.map(kernel_version, 8);
                memory.store<uint16_t>(kernel_version, 2);
                memory.store<uint16_t>(kernel_version + 2, 0);
                memory.store<uint16_t>(kernel_version + 4, 12416);
                memory.store<uint16_t>(kernel_version + 6, 0);
                memory.store<uint32_t>(value, kernel_version);
                std::cerr << "BIND XboxKrnlVersion address=0x71210000 version=2.0.12416.0 source=verified-game-abi\n";
            }
            else if (name == "__imp__KeTimeStampBundle" && value == 0x82000758) {
                memory.store<uint32_t>(value, sfr::TimestampBundle::address);
                std::cerr << "BIND KeTimeStampBundle address=0x" << std::hex << sfr::TimestampBundle::address
                          << std::dec << " uptime_ms=" << memory.load<uint32_t>(sfr::TimestampBundle::address + 16) << '\n';
            }
            else
                memory.add_import_variable(static_cast<uint32_t>(value), name);
            ++variable_count;
        }
        if (variable_count == 0) throw std::runtime_error("no import variables registered");
        for (auto* mapping = PPCFuncMappings; mapping->host; ++mapping) {
            if (mapping->guest < PPC_CODE_BASE || mapping->guest >= PPC_CODE_BASE + PPC_CODE_SIZE)
                throw std::runtime_error("function table address outside guest code");
            sfr::functions.emplace(static_cast<uint32_t>(mapping->guest), mapping->host);
        }
        const uint32_t tls_metadata = module.header_field(sfr::XexModule::header_address, 0x20104);
        if (!tls_metadata) throw std::runtime_error("diagnostic requires the title's TLS metadata");
        memory.check(tls_metadata, 16);
        const uint32_t tls_slots = memory.load<uint32_t>(tls_metadata);
        const uint32_t tls_raw_address = memory.load<uint32_t>(uint64_t(tls_metadata) + 4);
        const uint32_t tls_data_size = memory.load<uint32_t>(uint64_t(tls_metadata) + 8);
        const uint32_t tls_raw_size = memory.load<uint32_t>(uint64_t(tls_metadata) + 12);
        sfr::ThreadLocalStorage tls_storage(memory, tls_slots, tls_raw_address, tls_data_size, tls_raw_size);
        sfr::thread_local_storage = &tls_storage;
        sfr::current_tls_dynamic = tls_storage.dynamic_address();
        std::cerr << "TLS static=0x" << std::hex << sfr::ThreadLocalStorage::static_address
                  << " dynamic=0x" << tls_storage.dynamic_address() << std::dec << " slots=" << tls_slots
                  << " data_size=" << tls_data_size << " raw_size=" << tls_raw_size << '\n';
        // Main-thread storage; workers receive independent PCR, TLS and stacks.
        constexpr uint32_t pcr = sfr::diagnostic_pcr, teb = sfr::diagnostic_thread;
        constexpr uint32_t stack = 0x70100000, stack_size = 0x40000;
        memory.map(pcr, 0x3000);
        memory.map(stack, stack_size);
        memory.store<uint32_t>(pcr, sfr::ThreadLocalStorage::static_address);
        memory.store<uint32_t>(pcr + 0x100, teb);
        memory.store<uint32_t>(teb + 0x68, sfr::ThreadLocalStorage::static_address);
        memory.store<uint32_t>(teb + 0x14c, 1);
        const uint64_t tls_backing_size = (uint64_t(tls_data_size) + uint64_t(tls_slots) * 4 + 4095) & ~uint64_t(4095);
        sfr::PhysicalMemory physical(memory);
        sfr::physical_memory = &physical;
        sfr::GuestExecution execution;
        // Guest threads run one at a time; a 2 ms quantum keeps handoffs rare
        // enough for the title to run at speed while others still progress.
        execution.set_scheduling_quantum(std::chrono::microseconds(2000));
        for (auto& core : sfr::core_executions) {
            core = std::make_unique<sfr::GuestExecution>();
            core->set_scheduling_quantum(std::chrono::microseconds(2000));
            execution.add_follower(*core);  // a guest blocked on its core wakes at shutdown
        }
        // SFR_MAIN_URGENT=1 (experiment, off): make the title's main thread
        // (guest 1) time-critical. It spends about a third of a race frame
        // ready but queued behind a job worker for the one permit
        // (docs/performance.md), and this would let it take the permit back.
        // It deadlocks in the menus: a guest sets an event another guest waits
        // on and every thread then waits (out/urgent-hang.log, present 5092).
        if (const char* text = std::getenv("SFR_MAIN_URGENT"); text && *text == '1')
            execution.set_urgent(1, true);
        sfr::runtime_shader_translation = true;
        sfr::execution = &execution;
        sfr::NativeGraphics graphics;
        sfr::native_graphics = &graphics;
        sfr::GuestGraphics guest_graphics(memory, graphics);
        sfr::active_guest_graphics = &guest_graphics;
        // The main thread waits for the GPU at every present while holding the
        // guest permit, which leaves the job worker queued behind a thread
        // that is only waiting. Release the permit around that wait, as a
        // guest's own waits do. Only guest 1 renders; a wait outside guest
        // execution (shutdown) just waits. SFR_GPU_WAIT_RELEASE=0 keeps the
        // permit.
        if (const char* text = std::getenv("SFR_GPU_WAIT_RELEASE"); !text || *text != '0')
            sfr::NativePresentation::set_gpu_wait([](const std::function<void()>& wait) {
                if (!sfr::execution_permit || sfr::current_id != 1) { wait(); return; }
                const auto started = std::chrono::steady_clock::now();
                sfr::execution_permit->run_blocking([&](std::stop_token) { wait(); }, {});
                sfr::main_gpu_wait_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - started).count()), std::memory_order_relaxed);
            });
        sfr::NativeSyncObjects sync_objects;
        sfr::NativeNotifications notifications(memory, sync_objects);
        sfr::native_notifications = &notifications;
        sfr::NotificationPlacement notification_placement;
        sfr::notification_placement = &notification_placement;
        sfr::GuestSyncObjects sync_adapter(memory, sync_objects);
        sfr::native_sync_objects = &sync_objects;
        sfr::guest_sync_objects = &sync_adapter;
        sfr::GuestThreads threads(memory, {tls_slots, tls_raw_address, tls_data_size, tls_raw_size},
            module.header_field(sfr::XexModule::header_address, 0x20200),
            [](uint32_t address) { return sfr::functions.contains(address); },
            [&memory, &execution, &tls_storage](const sfr::GuestThreads::State& state) -> sfr::NativeThread::Entry {
                tls_storage.register_thread(state.tls_dynamic);
                auto context = std::make_shared<PPCContext>();
                context->r1.u64 = state.stack_base - 0x100;
                context->r13.u64 = state.pcr;
                context->r3.u64 = state.worker;
                context->r4.u64 = state.argument;
                return [context, &memory, &execution, state](std::stop_token stop) -> uint32_t {
                    if (stop.stop_requested()) return 0;
                    std::unique_ptr<sfr::GuestExecution::Lease> permit;
                    try {
                        permit = execution.enter(state.id);
                        sfr::execution_permit = permit.get();
                        sfr::current_context = context.get();
                        sfr::current_pcr = state.pcr;
                        sfr::current_thread = state.thread_object;
                        sfr::current_id = state.id;
                        sfr::current_tls = state.tls_static;
                        sfr::current_tls_dynamic = state.tls_dynamic;
#ifdef _WIN32
                        if (state.id < 64 && std::getenv("SFR_PROFILE_GUEST"))
                            sfr::guest_host_threads[state.id] = sfr::own_thread_handle();
#endif
                        // Resumed by another guest: let it reach its next wait
                        // before running any guest code (the object it resumed
                        // us for may still be under construction; see
                        // GuestExecution::wait_for_block). The permit goes back
                        // to it meanwhile.
                        if (state.startup) {
                            std::optional<sfr::ThreadResumer> resumer;
                            {
                                std::lock_guard lock(sfr::thread_resumers_lock);
                                if (const auto found = sfr::thread_resumers.find(state.handle);
                                    found != sfr::thread_resumers.end()) {
                                    resumer = found->second;
                                    sfr::thread_resumers.erase(found);
                                }
                            }
                            if (resumer && resumer->guest_id != state.id)
                                permit->run_blocking([&](std::stop_token token) {
                                    sfr::GuestExecution::wait_for_block(resumer->guest_id, resumer->blocks,
                                                                        std::chrono::milliseconds(20), token);
                                });
                        }
                        const unsigned processor = memory.load<uint8_t>(uint64_t(state.pcr) + 0x10C);
                        const bool on_core = sfr::parallel_worker == sfr::ParallelGuests::cores &&
                            processor != 0 && processor < sfr::guest_processors;
                        if (state.startup && (on_core || sfr::parallel_worker == sfr::ParallelGuests::all ||
                                (sfr::parallel_worker == sfr::ParallelGuests::job_worker &&
                                 state.worker == sfr::parallel_worker_entry))) {
                            std::cerr << "PARALLEL_WORKER guest_id=" << state.id << " processor=" << processor << '\n';
                            sfr::parallel_guest = true;
                            sfr::GuestMemory::concurrent_reader = true;
                            sfr::GuestMemory::slow_access_hook = sfr::parallel_slow_access;
                            permit->detach();
                            if (on_core) {
                                // Global permit released first: a core is only
                                // ever awaited without the global permit.
                                sfr::core_index = processor;
                                sfr::core_permit = sfr::core_executions[processor]->enter(state.id);
                                // The core is taken after the global permit, and
                                // released whenever this thread waits for it.
                                permit->set_companion(sfr::core_permit.get());
                                permit->set_after_blocking([] {
                                    // Moved to another processor meanwhile: continue there.
                                    const unsigned now = sfr::active_memory->load<uint8_t>(uint64_t(sfr::current_pcr) + 0x10C);
                                    if (now != sfr::core_index && now != 0 && now < sfr::guest_processors) {
                                        sfr::execution_permit->set_companion(nullptr);
                                        sfr::core_permit.reset();
                                        sfr::core_index = now;
                                        sfr::core_permit = sfr::core_executions[now]->enter(sfr::current_id);
                                        sfr::execution_permit->set_companion(sfr::core_permit.get());
                                    }
                                });
                            }
                        }
                        sfr::entry_observed = sfr::needs_entry_observation();
                        context->fpscr.loadFromHost();
                        if (!state.startup) {
                            // Audio render driver: like the console's audio
                            // hardware, request one 256-sample frame (48 kHz)
                            // every 5.33 ms from the registered client callback.
                            std::cerr << "NATIVE_AUDIO_PUMP_BEGIN guest_id=" << state.id << '\n';
                            execution.set_urgent(state.id, true);
                            using clock = std::chrono::steady_clock;
                            constexpr auto period = std::chrono::nanoseconds(16'000'000 / 3);
                            auto next = clock::now();
                            for (;;) {
                                // Frames that fell due while this thread waited for
                                // the execution permit are rendered back to back
                                // (bounded), so the audio clock - which paces the
                                // movie decoder - keeps real time.
                                const auto due = clock::now() - next;
                                const int64_t frames = due < clock::duration::zero() ? 1
                                    : std::min<int64_t>(due / period + 1, 32);
                                for (int64_t frame = 0; frame < frames; ++frame, next += period) {
                                    const auto client = sfr::audio_client;
                                    if (!client.routine) continue;
                                    // The callback receives a pointer to a word holding
                                    // the registered context (Xenia's wrapped argument);
                                    // the word sits above the callback's stack frame.
                                    const uint32_t wrapped = state.stack_base - 8;
                                    memory.store<uint32_t>(wrapped, client.context);
                                    context->r1.u64 = state.stack_base - 0x100;
                                    context->r13.u64 = state.pcr;
                                    context->r3.u64 = wrapped;
                                    sfr::call_indirect(*context, memory.base(), client.routine);
                                    ++sfr::audio_frames_requested;
                                }
                                const auto now = clock::now();
                                if (next + std::chrono::milliseconds(500) < now) next = now;  // drop a long stall
                                permit->run_blocking([next](std::stop_token token) {
                                    while (!token.stop_requested() && clock::now() < next)
                                        std::this_thread::sleep_for(std::min<clock::duration>(next - clock::now(),
                                                                                              std::chrono::milliseconds(2)));
                                });
                            }
                        }
                        std::cerr << "ORIGINAL_WORKER_BEGIN guest_id=" << state.id
                                  << " host_thread=" << std::this_thread::get_id()
                                  << " startup=0x" << std::hex << state.startup << " worker=0x" << state.worker
                                  << " argument=0x" << state.argument << " pcr=0x" << state.pcr << std::dec << '\n';
                        sfr::call_indirect(*context, memory.base(), state.startup);
                        throw sfr::RuntimeStop("thread-return", state.startup,
                            "original XAPI startup returned without thread termination");
                    } catch (const sfr::GuestThreadExit& exit) {
                        // ExTerminateThread: the guest stack unwinds and the host
                        // thread ends, signaling waiters on the thread handle.
                        std::cerr << "ORIGINAL_WORKER_EXIT guest_id=" << state.id << " code=" << exit.code << '\n';
                        permit.reset();
                        sfr::execution_permit = nullptr;
                        sfr::core_permit.reset();
                        sfr::current_context = nullptr;
                        return exit.code;
                    } catch (const sfr::GuestExecutionCancelled&) {
                        // Another thread or shutdown already owns the first failure.
                    } catch (const sfr::RuntimeStop& error) {
                        const auto cause = std::current_exception();
                        try {
                            std::ostringstream trace;
                            trace << error.detail << " [guest_id=" << state.id << " function=" << sfr::current_function
                                  << " address=0x" << std::hex << sfr::current_address << " LR=0x" << context->lr
                                  << " r1=0x" << context->r1.u32 << " r3=0x" << context->r3.u32
                                  << " r10=0x" << context->r10.u32 << " r11=0x" << context->r11.u32
                                  << " r29=0x" << context->r29.u32 << " r31=0x" << context->r31.u32
                                  << " stack=" << sfr::guest_back_chain(context->r1.u32) << ']';
                            execution.fail(std::make_exception_ptr(sfr::RuntimeStop("worker-" + error.category, error.address, trace.str())));
                        } catch (...) { execution.fail(cause); }
                    } catch (...) {
                        execution.fail(std::current_exception());
                    }
                    sfr::execution_permit = nullptr;
                    sfr::core_permit.reset();
                    sfr::current_context = nullptr;
                    return 0;
                };
            });
        sfr::guest_threads = &threads;
        std::unique_ptr<sfr::AssetFiles> assets;
        std::unique_ptr<sfr::GuestFiles> files;
        std::unique_ptr<sfr::GuestAsyncFiles> async_files;
        if (mount_assets) {
            assets = std::make_unique<sfr::AssetFiles>(std::filesystem::path(argv[2]));
            files = std::make_unique<sfr::GuestFiles>(memory, *assets);
            sfr::asset_files = assets.get();
            sfr::guest_files = files.get();
            async_files=std::make_unique<sfr::GuestAsyncFiles>(memory,*assets,sync_objects,execution,
                [&memory](const sfr::GuestFiles::ReadRequest& request,const sfr::GuestFiles::ReadResult& result){
                    std::cerr << "NATIVE_ASYNC_FILE_DATA handle=0x" << std::hex << request.handle
                              << " event=0x" << request.event << " io=0x" << request.io_output
                              << " buffer=0x" << request.buffer << std::dec << " offset=" << result.offset
                              << " requested=" << request.length << " transferred=" << result.transferred
                              << " status=0x" << std::hex << result.status << std::dec;
                    if(result.transferred)
                        std::cerr << " sha1=" << fingerprint(memory.base()+request.buffer,result.transferred);
                    std::cerr << '\n';
                });
            sfr::guest_async_files=async_files.get();
        }
        // Saves live in SFR_SAVE_DIRECTORY (default "save" in the working directory).
        const char* save_directory = std::getenv("SFR_SAVE_DIRECTORY");
        sfr::ContentFiles content(memory, save_directory && *save_directory ? save_directory : "save");
        sfr::content_files = &content;
        sfr::MemoryStatistics statistics(memory, allocations, physical, {PPC_IMAGE_BASE, PPC_IMAGE_SIZE},
            {stack, stack_size}, {sfr::ThreadLocalStorage::static_address, tls_backing_size});
        sfr::memory_statistics = &statistics;
        ctx.r1.u64 = stack + stack_size - 0x100;
        ctx.r13.u64 = pcr;
        ctx.fpscr.loadFromHost();
        {
            struct Shutdown {
                sfr::GuestExecution& execution;
                sfr::GuestThreads& threads;
                sfr::GuestAsyncFiles* async_files;
                ~Shutdown() {
                    execution.stop_and_drain();
                    threads.shutdown();
                    if(async_files)async_files->shutdown();
                }
            } shutdown{execution
                , threads, async_files.get()
            };
#ifdef _WIN32
            std::jthread host_profiler([](std::stop_token stop) {
                const bool main_only = std::getenv("SFR_MAIN_PROFILE") != nullptr;
                const unsigned profiled = [] {
                    const char* text = std::getenv("SFR_PROFILE_GUEST");
                    const unsigned id = text ? unsigned(std::strtoul(text, nullptr, 10)) : 1u;
                    return id < 64 ? id : 1u;
                }();
                if (!main_only && !std::getenv("SFR_HOST_PROFILE")) return;
                std::unordered_map<uint64_t, uint64_t> samples;
                const auto module = uint64_t(GetModuleHandleW(nullptr));
                while (!stop.stop_requested()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    if (sfr::present_count < sfr::profile_after) continue;
                    const HANDLE thread = (main_only ? sfr::guest_host_threads[profiled] : sfr::profile_host_thread)
                        .load(std::memory_order_relaxed);
                    if (!thread || SuspendThread(thread) == DWORD(-1)) continue;
                    // Nothing that may take a lock (allocation, I/O) while the
                    // target is suspended: it could hold that lock.
                    CONTEXT context{};
                    context.ContextFlags = CONTEXT_CONTROL;
                    const bool sampled = GetThreadContext(thread, &context);
                    ResumeThread(thread);
                    if (sampled) ++samples[context.Rip - module];
                }
                std::vector<std::pair<uint64_t, uint64_t>> ranked;
                for (const auto& [offset, count] : samples) ranked.emplace_back(count, offset);
                std::sort(ranked.rbegin(), ranked.rend());
                for (const auto& [count, offset] : ranked)
                    std::cerr << "HOST_PROFILE rva=0x" << std::hex << offset << std::dec << ' ' << count << '\n';
            });
#endif
            std::jthread profiler([](std::stop_token stop) {
                if (!std::getenv("SFR_SAMPLE_PROFILE")) return;
                std::unordered_map<uint64_t, uint64_t> samples;
                uint64_t total = 0;
                while (!stop.stop_requested()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    // SFR_PROFILE_AFTER=N samples only from the Nth present,
                    // so a race is measured without its loading screens.
                    if (sfr::present_count < sfr::profile_after) continue;
                    ++samples[sfr::profile_address.load(std::memory_order_relaxed)];
                    ++total;
                }
                std::vector<std::pair<uint64_t, uint64_t>> ranked;
                for (const auto& [address, count] : samples) ranked.emplace_back(count, address);
                std::sort(ranked.rbegin(), ranked.rend());
                std::cerr << "PROFILE samples=" << total << '\n';
                for (size_t i = 0; i < ranked.size() && i < 60; ++i)
                    std::cerr << "PROFILE thread=" << (ranked[i].second >> 32) << " 0x" << std::hex
                              << uint32_t(ranked[i].second) << std::dec << ' ' << ranked[i].first << '\n';
            });
            std::jthread watchdog([&execution](std::stop_token stop) {
                for (uint64_t i = 0; i < sfr::watchdog_seconds * 10 && !stop.stop_requested(); ++i) {
                    // One second before the timeout, running guest threads print their stacks.
                    if (i + 10 == sfr::watchdog_seconds * 10) sfr::stack_dump_requested = true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (!stop.stop_requested())
                    execution.fail(std::make_exception_ptr(sfr::RuntimeStop("timeout", 0,
                        "CPU diagnostic exceeded " + std::to_string(sfr::watchdog_seconds) + " seconds")));
            });
            auto permit = execution.enter(1);
            sfr::execution_permit = permit.get();
            sfr::current_context = &ctx;
            sfr::entry_observed = sfr::needs_entry_observation();
#ifdef _WIN32
            sfr::guest_host_threads[1] = sfr::own_thread_handle();
#endif
            try {
                std::cerr << "Free Riders CPU diagnostic: entry=0x824D22F0 mappings=" << sfr::functions.size()
                          << " import_variables=" << variable_count << '\n';
                std::cerr << "DIAGNOSTIC_LIMITS calls=" << sfr::call_budget << " watchdog_seconds=" << sfr::watchdog_seconds << '\n';
                _xstart(ctx, memory.base());
            } catch (const sfr::GuestExecutionCancelled&) {
            } catch (...) {
                execution.fail(std::current_exception());
            }
            sfr::execution_permit = nullptr;
            sfr::current_context = nullptr;
        } // Permit released, then workers stopped/joined before their dependencies.
        execution.rethrow_failure();
        std::cerr << "STOP entry returned; no gameplay success is implied\n";
        return 4;
    } catch (const sfr::RuntimeStop& error) {
        std::cerr << "STOP " << error.category << " @0x" << std::hex << error.address << ": " << error.detail
                  << "\nLAST_FUNCTION " << sfr::current_function << " @0x" << sfr::current_address
                  << " LR=0x" << ctx.lr << std::dec << " calls=" << sfr::calls << '\n';
        return 3;
    } catch (const std::exception& error) {
        std::cerr << "Diagnostic error: " << error.what() << '\n';
        return 1;
    }
}
