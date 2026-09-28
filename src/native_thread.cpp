#include "native_thread.h"

#include "guest_memory.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sfr {

namespace {

[[noreturn]] void throw_host_error(const char* operation, DWORD error) {
    throw RuntimeStop("thread-host", error,
        std::string(operation) + " failed with Windows error " + std::to_string(error));
}

// The allowed logical processors of one group in the order guest processors
// take them: the first logical processor of every physical core, fastest
// cores (highest efficiency class) first, and only then the second hardware
// thread of each core. Taking bits in order instead put the six guest
// processors on three cores, two hardware threads each, on a machine with
// simultaneous multithreading (logical 0 and 1 are one core). The ordering
// is a host choice only: each guest processor still gets one processor of
// its own. SFR_HOST_PROCESSORS=sequential restores the plain bit order.
std::vector<uint32_t> processors_by_core(uint64_t allowed, uint16_t group) {
    std::vector<uint32_t> sequential;
    for (uint32_t bit = 0; bit < 64; ++bit)
        if (allowed >> bit & 1) sequential.push_back(bit);
    const char* setting = std::getenv("SFR_HOST_PROCESSORS");
    if (setting && std::string_view(setting) == "sequential") return sequential;
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || length == 0) return sequential;
    std::vector<unsigned char> buffer(length);
    auto* first = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, first, &length)) return sequential;
    struct Core { BYTE efficiency; std::vector<uint32_t> threads; };
    std::vector<Core> cores;
    for (DWORD offset = 0; offset < length;) {
        const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
        if (entry->Size == 0) break;
        if (entry->Relationship == RelationProcessorCore) {
            Core core{entry->Processor.EfficiencyClass, {}};
            for (WORD i = 0; i < entry->Processor.GroupCount; ++i) {
                const GROUP_AFFINITY& mask = entry->Processor.GroupMask[i];
                if (mask.Group != group) continue;
                for (uint32_t bit = 0; bit < 64; ++bit)
                    if ((uint64_t(mask.Mask) & allowed) >> bit & 1) core.threads.push_back(bit);
            }
            if (!core.threads.empty()) cores.push_back(std::move(core));
        }
        offset += entry->Size;
    }
    std::stable_sort(cores.begin(), cores.end(),
                     [](const Core& a, const Core& b) { return a.efficiency > b.efficiency; });
    std::vector<uint32_t> ordered;
    for (size_t thread = 0; ordered.size() < sequential.size(); ++thread) {
        const size_t before = ordered.size();
        for (const Core& core : cores)
            if (thread < core.threads.size()) ordered.push_back(core.threads[thread]);
        if (ordered.size() == before) break;
    }
    // Anything the core list missed keeps its place at the end.
    for (const uint32_t bit : sequential)
        if (std::find(ordered.begin(), ordered.end(), bit) == ordered.end()) ordered.push_back(bit);
    return ordered;
}

}

struct NativeThread::Impl {
    Entry entry;
    std::stop_source stop_source;
    HANDLE handle = nullptr;
    uint32_t id = 0;
    std::atomic<bool> started = false;
    std::atomic<bool> completed = false;
    std::exception_ptr failure;
    uint32_t exit_code = 0;
    bool is_suspended = true;
    bool joined = false;
    uint64_t allowed_affinity = 0;
    uint16_t processor_group = 0;

    Impl(Entry callback, uint64_t allowed, uint16_t group)
        : entry(std::move(callback)), allowed_affinity(allowed), processor_group(group) {}

    static DWORD WINAPI trampoline(void* raw) noexcept {
        auto& self = *static_cast<Impl*>(raw);
        uint32_t result = 0;
        if (!self.stop_source.stop_requested()) {
            self.started.store(true, std::memory_order_release);
            try {
                result = self.entry(self.stop_source.get_token());
            } catch (...) {
                self.failure = std::current_exception();
            }
        }
        self.exit_code = result;
        self.completed.store(true, std::memory_order_release);
        return result;
    }
};

NativeThread::NativeThread(Entry entry) {
    if (!entry) throw RuntimeStop("thread-host", 0, "native thread entry is empty");

    USHORT group_count = 1;
    USHORT groups[1]{};
    if (!GetProcessGroupAffinity(GetCurrentProcess(), &group_count, groups)) {
        const DWORD error = GetLastError();
        if (error == ERROR_INSUFFICIENT_BUFFER || group_count != 1)
            throw RuntimeStop("thread-host", group_count,
                "native thread affinity requires a process in exactly one processor group");
        throw_host_error("GetProcessGroupAffinity", error);
    }
    if (group_count != 1)
        throw RuntimeStop("thread-host", group_count,
            "native thread affinity requires a process in exactly one processor group");

    DWORD_PTR process_mask = 0;
    DWORD_PTR system_mask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask))
        throw_host_error("GetProcessAffinityMask", GetLastError());
    if (process_mask == 0)
        throw RuntimeStop("thread-host", 0, "process has no allowed processors in its primary group");

    auto state = std::make_unique<Impl>(std::move(entry), process_mask, groups[0]);
    DWORD id = 0;
    state->handle = CreateThread(nullptr, 16u * 1024u * 1024u, &Impl::trampoline, state.get(),
        CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, &id);
    if (!state->handle) throw_host_error("CreateThread", GetLastError());
    state->id = id;

    GROUP_AFFINITY affinity{};
    const bool affinity_queried = GetThreadGroupAffinity(state->handle, &affinity) != FALSE;
    const DWORD affinity_error = affinity_queried ? ERROR_SUCCESS : GetLastError();
    if (!affinity_queried || affinity.Group != state->processor_group) {
        state->stop_source.request_stop();
        if (ResumeThread(state->handle) == static_cast<DWORD>(-1) ||
            WaitForSingleObject(state->handle, INFINITE) != WAIT_OBJECT_0 ||
            !state->completed.load(std::memory_order_acquire) ||
            !CloseHandle(state->handle)) std::terminate();
        if (!affinity_queried) throw_host_error("GetThreadGroupAffinity", affinity_error);
        throw RuntimeStop("thread-host", affinity.Group,
            "new thread primary processor group does not match the process group");
    }
    impl_ = std::move(state);
}

NativeThread::~NativeThread() noexcept {
    if (!impl_) return;
    cancel_and_join();
    if (!CloseHandle(impl_->handle)) std::terminate();
}

uint32_t NativeThread::native_id() const { return impl_->id; }
bool NativeThread::entry_started() const { return impl_->started.load(std::memory_order_acquire); }
bool NativeThread::suspended() const { return impl_->is_suspended; }
void* NativeThread::native_handle() const { return impl_->handle; }

uint32_t NativeThread::resume() {
    const DWORD previous = ResumeThread(impl_->handle);
    if (previous == static_cast<DWORD>(-1)) throw_host_error("ResumeThread", GetLastError());
    if (previous == 1) impl_->is_suspended = false;
    return previous;
}

uint32_t NativeThread::join() {
    if (impl_->is_suspended)
        throw RuntimeStop("thread-host", impl_->id, "cannot join a thread that has never been resumed");

    if (!impl_->joined) {
        const DWORD wait = WaitForSingleObject(impl_->handle, INFINITE);
        if (wait != WAIT_OBJECT_0) throw_host_error("WaitForSingleObject", GetLastError());

        DWORD result = 0;
        if (!GetExitCodeThread(impl_->handle, &result))
            throw_host_error("GetExitCodeThread", GetLastError());
        if (!impl_->completed.load(std::memory_order_acquire))
            throw RuntimeStop("thread-host", impl_->id, "thread exited without publishing completion");
        impl_->exit_code = result;
        impl_->joined = true;
    }

    if (impl_->failure) std::rethrow_exception(impl_->failure);
    return impl_->exit_code;
}

void NativeThread::request_stop() noexcept { impl_->stop_source.request_stop(); }

void NativeThread::cancel_and_join() noexcept {
    request_stop();
    if (impl_->joined) return;
    if (impl_->is_suspended) {
        if (ResumeThread(impl_->handle) == static_cast<DWORD>(-1)) std::terminate();
        impl_->is_suspended = false;
    }
    if (WaitForSingleObject(impl_->handle, INFINITE) != WAIT_OBJECT_0) std::terminate();
    if (!impl_->completed.load(std::memory_order_acquire)) std::terminate();
    impl_->joined = true;
}

int32_t NativeThread::priority() const {
    const int value = GetThreadPriority(impl_->handle);
    if (value == THREAD_PRIORITY_ERROR_RETURN)
        throw_host_error("GetThreadPriority", GetLastError());
    return value;
}

int32_t NativeThread::set_priority(int32_t host_relative) {
    if (host_relative < THREAD_PRIORITY_LOWEST || host_relative > THREAD_PRIORITY_HIGHEST)
        throw RuntimeStop("thread-host", static_cast<uint32_t>(host_relative),
            "thread priority must be one of -2, -1, 0, 1, or 2");
    const int previous = GetThreadPriority(impl_->handle);
    if (previous == THREAD_PRIORITY_ERROR_RETURN)
        throw_host_error("GetThreadPriority", GetLastError());
    if (!SetThreadPriority(impl_->handle, host_relative))
        throw_host_error("SetThreadPriority", GetLastError());
    return previous;
}

uint64_t NativeThread::set_guest_processor(uint32_t guest_cpu) {
    if (guest_cpu >= 6)
        throw RuntimeStop("thread-host", guest_cpu, "guest processor index must be below 6");
    (void)affinity_mask();

    static std::mutex order_lock;
    static uint64_t ordered_for = 0;
    static uint16_t ordered_group = 0;
    static std::vector<uint32_t> ordered;
    std::vector<uint32_t> processors;
    {
        std::lock_guard guard(order_lock);
        if (ordered.empty() || ordered_for != impl_->allowed_affinity || ordered_group != impl_->processor_group) {
            ordered = processors_by_core(impl_->allowed_affinity, impl_->processor_group);
            ordered_for = impl_->allowed_affinity;
            ordered_group = impl_->processor_group;
            std::cerr << "NATIVE_HOST_PROCESSORS group=" << impl_->processor_group << " order=";
            for (size_t i = 0; i < ordered.size(); ++i) std::cerr << (i ? "," : "") << ordered[i];
            std::cerr << '\n';
        }
        processors = ordered;
    }
    if (processors.empty())
        throw RuntimeStop("thread-host", 0, "cached process affinity mask is empty");
    const uint64_t selected_mask = uint64_t{1} << processors[guest_cpu % processors.size()];
    if (selected_mask == 0)
        throw RuntimeStop("thread-host", guest_cpu, "could not select an allowed host processor");
    if (SetThreadAffinityMask(impl_->handle, static_cast<DWORD_PTR>(selected_mask)) == 0)
        throw_host_error("SetThreadAffinityMask", GetLastError());
    return selected_mask;
}

uint64_t NativeThread::affinity_mask() const {
    GROUP_AFFINITY affinity{};
    if (!GetThreadGroupAffinity(impl_->handle, &affinity))
        throw_host_error("GetThreadGroupAffinity", GetLastError());
    if (affinity.Group != impl_->processor_group)
        throw RuntimeStop("thread-host", affinity.Group,
            "thread moved outside its validated processor group");
    return affinity.Mask;
}

}
