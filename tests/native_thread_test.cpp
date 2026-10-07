#include "native_thread.h"

#include "guest_memory.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition) {
    if (!condition) throw std::runtime_error("native thread test assertion failed");
}

DWORD process_handle_count() {
    DWORD count = 0;
    require(GetProcessHandleCount(GetCurrentProcess(), &count) != FALSE);
    return count;
}

uint64_t allowed_process_mask() {
    DWORD_PTR process = 0;
    DWORD_PTR system = 0;
    require(GetProcessAffinityMask(GetCurrentProcess(), &process, &system) != FALSE);
    require(process != 0);
    return process;
}

// Guest processors take the allowed processors in host_processor_order.
uint64_t selected_mask(uint64_t allowed, uint32_t guest_cpu) {
    const std::vector<uint32_t> order = sfr::host_processor_order(allowed, 0);
    require(!order.empty());
    return uint64_t{1} << order[sfr::host_processor_slot(guest_cpu, order.size())];
}

// The physical core of each logical processor of group 0.
std::vector<int> core_of_processors() {
    std::vector<int> core(64, -1);
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    std::vector<unsigned char> buffer(length);
    auto* first = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
    require(GetLogicalProcessorInformationEx(RelationProcessorCore, first, &length) != FALSE);
    int index = 0;
    for (DWORD offset = 0; offset < length; ++index) {
        const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
        for (WORD g = 0; g < entry->Processor.GroupCount; ++g)
            if (entry->Processor.GroupMask[g].Group == 0)
                for (uint32_t bit = 0; bit < 64; ++bit)
                    if (uint64_t(entry->Processor.GroupMask[g].Mask) >> bit & 1) core[bit] = index;
        offset += entry->Size;
    }
    return core;
}

uint64_t queried_thread_affinity(uint32_t native_id) {
    HANDLE handle = OpenThread(THREAD_QUERY_INFORMATION, FALSE, native_id);
    require(handle != nullptr);
    GROUP_AFFINITY affinity{};
    const bool queried = GetThreadGroupAffinity(handle, &affinity) != FALSE;
    const bool closed = CloseHandle(handle) != FALSE;
    require(queried);
    require(closed);
    return affinity.Mask;
}

void construction_parks_the_host_thread() {
    std::atomic<uint32_t> calls = 0;
    sfr::NativeThread thread([&](std::stop_token) {
        ++calls;
        return 0;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(thread.native_id() != 0);
    require(thread.suspended());
    require(!thread.entry_started());
    require(calls.load() == 0);
}

void resume_runs_on_the_reported_native_thread_and_join_returns_its_code() {
    std::atomic<uint32_t> callback_id = 0;
    sfr::NativeThread thread([&](std::stop_token) {
        callback_id.store(GetCurrentThreadId());
        return 0x89abcdefu;
    });

    const auto native_id = thread.native_id();
    require(thread.resume() == 1);
    require(!thread.suspended());
    require(thread.join() == 0x89abcdefu);
    require(thread.entry_started());
    require(callback_id.load() == native_id);
}

void repeated_resume_reports_the_actual_suspend_count() {
    std::atomic<bool> release = false;
    sfr::NativeThread thread([&](std::stop_token stop) {
        while (!release.load() && !stop.stop_requested()) std::this_thread::yield();
        return 7;
    });

    require(thread.resume() == 1);
    require(thread.resume() == 0);
    release.store(true);
    require(thread.join() == 7);
}

void joining_a_never_resumed_thread_is_rejected() {
    sfr::NativeThread thread([](std::stop_token) { return 0; });
    bool rejected = false;
    try { (void)thread.join(); }
    catch (const sfr::RuntimeStop& stop) { rejected = stop.category == "thread-host"; }
    require(rejected);
}

void callback_exceptions_are_rethrown_by_join() {
    sfr::NativeThread thread([](std::stop_token) -> uint32_t {
        throw std::runtime_error("callback failed");
    });
    require(thread.resume() == 1);

    bool propagated = false;
    try { (void)thread.join(); }
    catch (const std::runtime_error& error) {
        propagated = std::string(error.what()) == "callback failed";
    }
    require(propagated);
}

void parked_destruction_cancels_before_entry_and_closes_the_handle() {
    const auto baseline = process_handle_count();
    std::atomic<uint32_t> calls = 0;
    {
        sfr::NativeThread thread([&](std::stop_token) {
            ++calls;
            return 0;
        });
    }
    require(calls.load() == 0);
    require(process_handle_count() == baseline);
}

void active_destruction_requests_stop_and_joins() {
    std::atomic<bool> exited = false;
    {
        sfr::NativeThread thread([&](std::stop_token stop) {
            while (!stop.stop_requested()) std::this_thread::yield();
            exited.store(true);
            return 0;
        });
        require(thread.resume() == 1);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!thread.entry_started() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        require(thread.entry_started());
    }
    require(exited.load());
}

void empty_entry_is_rejected() {
    bool rejected = false;
    try { sfr::NativeThread thread({}); }
    catch (const sfr::RuntimeStop& stop) { rejected = stop.category == "thread-host"; }
    require(rejected);
}

void priority_changes_are_native_and_return_the_previous_value() {
    std::atomic<uint32_t> calls = 0;
    sfr::NativeThread thread([&](std::stop_token) { ++calls; return 0; });
    const int32_t initial = thread.priority();
    const int32_t changed = initial == THREAD_PRIORITY_ABOVE_NORMAL
        ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_ABOVE_NORMAL;

    require(thread.set_priority(changed) == initial);
    require(thread.priority() == changed);
    require(thread.set_priority(initial) == changed);
    require(thread.priority() == initial);
    require(thread.suspended());
    require(!thread.entry_started());
    require(calls.load() == 0);
}

void unsupported_priority_is_rejected_without_mutation() {
    sfr::NativeThread thread([](std::stop_token) { return 0; });
    const int32_t initial = thread.priority();
    bool rejected = false;
    try { (void)thread.set_priority(3); }
    catch (const sfr::RuntimeStop& stop) { rejected = stop.category == "thread-host"; }
    require(rejected);
    require(thread.priority() == initial);
}

// Every allowed processor appears once, and the first ones are each on a
// physical core of their own: guest processors do not share a core while
// another core is free.
void host_processor_order_spreads_over_physical_cores() {
    const uint64_t allowed = allowed_process_mask();
    const std::vector<uint32_t> order = sfr::host_processor_order(allowed, 0);
    uint64_t seen = 0;
    for (const uint32_t bit : order) {
        require(bit < 64 && (allowed >> bit & 1));
        require(!(seen >> bit & 1));
        seen |= uint64_t{1} << bit;
    }
    require(seen == allowed);
    const std::vector<int> core = core_of_processors();
    std::vector<int> cores;
    for (uint32_t bit = 0; bit < 64; ++bit)
        if ((allowed >> bit & 1) && std::find(cores.begin(), cores.end(), core[bit]) == cores.end()) cores.push_back(core[bit]);
    for (size_t i = 0; i < cores.size() && i < order.size(); ++i)
        for (size_t j = 0; j < i; ++j) require(core[order[i]] != core[order[j]]);
}

void calling_guest_thread_uses_allowed_core_mapping() {
    GROUP_AFFINITY original{};
    require(GetThreadGroupAffinity(GetCurrentThread(), &original) != FALSE);
    struct Restore {
        GROUP_AFFINITY original;
        ~Restore() { if (!SetThreadGroupAffinity(GetCurrentThread(), &original, nullptr)) std::terminate(); }
    } restore{original};
    const auto order = sfr::host_processor_order(original.Mask, original.Group);
    require(!order.empty());
    for (uint32_t cpu = 0; cpu < 6; ++cpu) {
        require(SetThreadGroupAffinity(GetCurrentThread(), &original, nullptr) != FALSE);
        const uint64_t expected = uint64_t{1} << order[sfr::host_processor_slot(cpu, order.size())];
        require(sfr::pin_current_guest_processor(cpu) == expected);
        require(queried_thread_affinity(GetCurrentThreadId()) == expected);
    }
    // An existing thread restriction must not be widened back to the process mask.
    auto limited = original;
    limited.Mask = uint64_t{1} << order.back();
    require(SetThreadGroupAffinity(GetCurrentThread(), &limited, nullptr) != FALSE);
    require(sfr::pin_current_guest_processor(0) == limited.Mask);
    require(queried_thread_affinity(GetCurrentThreadId()) == limited.Mask);
    bool rejected = false;
    try { sfr::pin_current_guest_processor(6); }
    catch (const sfr::RuntimeStop& stop) { rejected = stop.category == "thread-host"; }
    require(rejected && queried_thread_affinity(GetCurrentThreadId()) == limited.Mask);
}

void explicit_cpu_sets_keep_the_calling_thread_affinity() {
    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    const auto get_thread = reinterpret_cast<decltype(&GetThreadSelectedCpuSets)>(GetProcAddress(kernel, "GetThreadSelectedCpuSets"));
    const auto get_process = reinterpret_cast<decltype(&GetProcessDefaultCpuSets)>(GetProcAddress(kernel, "GetProcessDefaultCpuSets"));
    const auto set_thread = reinterpret_cast<decltype(&SetThreadSelectedCpuSets)>(GetProcAddress(kernel, "SetThreadSelectedCpuSets"));
    const auto set_process = reinterpret_cast<decltype(&SetProcessDefaultCpuSets)>(GetProcAddress(kernel, "SetProcessDefaultCpuSets"));
    const auto get_system = reinterpret_cast<decltype(&GetSystemCpuSetInformation)>(GetProcAddress(kernel, "GetSystemCpuSetInformation"));
    if (!get_thread || !get_process || !set_thread || !set_process || !get_system) return;
    GROUP_AFFINITY original{};
    require(GetThreadGroupAffinity(GetCurrentThread(), &original) != FALSE);
    const auto order = sfr::host_processor_order(original.Mask, original.Group);
    if (order.size() < 2) return;
    ULONG size = 0;
    get_system(nullptr, 0, &size, GetCurrentProcess(), 0);
    std::vector<unsigned char> bytes(size);
    require(get_system(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(bytes.data()), size, &size, GetCurrentProcess(), 0) != FALSE);
    ULONG selected = 0;
    bool found = false;
    for (size_t offset = 0; offset < size;) {
        const auto* entry = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(bytes.data() + offset);
        require(entry->Size != 0 && entry->Size <= size - offset);
        if (entry->Type == CpuSetInformation && entry->CpuSet.Group == original.Group &&
            entry->CpuSet.LogicalProcessorIndex != order[0] &&
            (original.Mask & (uint64_t{1} << entry->CpuSet.LogicalProcessorIndex)) &&
            (!entry->CpuSet.Allocated || entry->CpuSet.AllocatedToTargetProcess)) {
            selected = entry->CpuSet.Id; found = true; break;
        }
        offset += entry->Size;
    }
    require(found);
    const auto read = [](auto query, HANDLE handle) {
        ULONG count = 0;
        query(handle, nullptr, 0, &count);
        std::vector<ULONG> sets(count);
        require(query(handle, sets.data(), count, &count) != FALSE);
        sets.resize(count);
        return sets;
    };
    struct Restore {
        GROUP_AFFINITY affinity;
        std::vector<ULONG> thread, process;
        decltype(&SetThreadSelectedCpuSets) set_thread;
        decltype(&SetProcessDefaultCpuSets) set_process;
        ~Restore() {
            if (!set_thread(GetCurrentThread(), thread.data(), ULONG(thread.size())) ||
                !set_process(GetCurrentProcess(), process.data(), ULONG(process.size())) ||
                !SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr)) std::terminate();
        }
    } restore{original, read(get_thread, GetCurrentThread()), read(get_process, GetCurrentProcess()), set_thread, set_process};
    require(set_process(GetCurrentProcess(), nullptr, 0) != FALSE);
    require(set_thread(GetCurrentThread(), &selected, 1) != FALSE);
    require(sfr::pin_current_guest_processor(0) == original.Mask);
    require(queried_thread_affinity(GetCurrentThreadId()) == original.Mask);
    require(read(get_thread, GetCurrentThread()) == std::vector<ULONG>{selected});
    require(set_thread(GetCurrentThread(), nullptr, 0) != FALSE);
    require(set_process(GetCurrentProcess(), &selected, 1) != FALSE);
    require(sfr::pin_current_guest_processor(0) == original.Mask);
    require(queried_thread_affinity(GetCurrentThreadId()) == original.Mask);
    require(read(get_process, GetCurrentProcess()) == std::vector<ULONG>{selected});
    sfr::NativeThread worker([](std::stop_token) { return 0; });
    const auto worker_mask = worker.affinity_mask();
    require(worker.set_guest_processor(0, true) == worker_mask);
    require(worker.affinity_mask() == worker_mask);
}

void six_guest_processors_map_to_allowed_native_processors() {
    const uint64_t allowed = allowed_process_mask();
    for (uint32_t guest_cpu = 0; guest_cpu < 6; ++guest_cpu) {
        std::atomic<uint32_t> calls = 0;
        sfr::NativeThread thread([&](std::stop_token) { ++calls; return 0; });
        const uint64_t expected = selected_mask(allowed, guest_cpu);
        require(thread.set_guest_processor(guest_cpu) == expected);
        require(thread.affinity_mask() == expected);
        require(queried_thread_affinity(thread.native_id()) == expected);
        require((expected & allowed) == expected);
        require((expected & (expected - 1)) == 0);
        require(thread.suspended());
        require(!thread.entry_started());
        require(calls.load() == 0);
    }
}

void migrating_workers_keep_their_guest_processor_preference() {
    const auto allowed = allowed_process_mask();
    const auto order = sfr::host_processor_order(allowed, 0);
    uint64_t pool = 0;
    for (size_t i = 0; i < (std::min)(size_t{6}, order.size()); ++i) pool |= uint64_t{1} << order[i];
    sfr::NativeThread worker([](std::stop_token) { return 0; });
    for (uint32_t cpu = 0; cpu < 6; ++cpu) {
        require(worker.set_guest_processor(cpu, true) == pool);
        require(worker.affinity_mask() == pool);
        PROCESSOR_NUMBER ideal{};
        require(GetThreadIdealProcessorEx(worker.native_handle(), &ideal) != FALSE);
        require(ideal.Group == 0 && ideal.Number == order[sfr::host_processor_slot(cpu, order.size())]);
        require(worker.suspended() && !worker.entry_started());
    }
    bool rejected = false;
    try { worker.set_guest_processor(6, true); }
    catch (const sfr::RuntimeStop& stop) { rejected = stop.category == "thread-host"; }
    require(rejected && worker.affinity_mask() == pool);
}

void invalid_guest_processor_is_rejected_without_mutation() {
    sfr::NativeThread thread([](std::stop_token) { return 0; });
    const uint64_t selected = thread.set_guest_processor(0);
    bool rejected = false;
    try { (void)thread.set_guest_processor(6); }
    catch (const sfr::RuntimeStop& stop) { rejected = stop.category == "thread-host"; }
    require(rejected);
    require(thread.affinity_mask() == selected);
    require(queried_thread_affinity(thread.native_id()) == selected);
}

void migrating_workers_respect_small_process_cpu_masks() {
    const auto allowed = allowed_process_mask();
    GROUP_AFFINITY original{};
    require(GetThreadGroupAffinity(GetCurrentThread(), &original) != FALSE);
    struct Restore {
        uint64_t process;
        GROUP_AFFINITY thread;
        ~Restore() {
            if (!SetProcessAffinityMask(GetCurrentProcess(), DWORD_PTR(process)) ||
                !SetThreadGroupAffinity(GetCurrentThread(), &thread, nullptr)) std::terminate();
        }
    } restore{allowed, original};
    const auto order = sfr::host_processor_order(allowed, original.Group);
    uint64_t limited = 0;
    // Exercise all six guest processors with only one or two available host
    // processors. The pool and preferred CPU must never escape the restriction.
    for (size_t i = 0; i < (std::min)(size_t{2}, order.size()); ++i) {
        limited |= uint64_t{1} << order[i];
        require(SetProcessAffinityMask(GetCurrentProcess(), DWORD_PTR(limited)) != FALSE);
        migrating_workers_keep_their_guest_processor_preference();
    }
}

// Fewer host processors than guest processors: guest processor 0 (the main
// thread's) keeps the first one to itself, and 1-5 take turns on the others
// (Issue #52: 4 % 4 put guest processor 4 on the main thread's core).
// The main thread is pinned only with six or more host processors
// (Issue #52: pinned on four, it waited for its core while the others idled).
void main_thread_is_pinned_only_with_six_or_more_processors() {
    for (size_t count = 0; count < 6; ++count) require(!sfr::pin_main_thread_by_default(count));
    for (size_t count = 6; count <= 64; ++count) require(sfr::pin_main_thread_by_default(count));
    GROUP_AFFINITY current{};
    require(GetThreadGroupAffinity(GetCurrentThread(), &current) != FALSE);
    require(sfr::current_host_processor_count() == sfr::host_processor_order(current.Mask, current.Group).size());
}

void host_processor_slots_leave_the_first_to_guest_processor_zero() {
    for (size_t count = 6; count <= 8; ++count)
        for (uint32_t cpu = 0; cpu < 6; ++cpu) require(sfr::host_processor_slot(cpu, count) == cpu);
    for (uint32_t cpu = 0; cpu < 6; ++cpu) require(sfr::host_processor_slot(cpu, 1) == 0);
    const uint32_t two[6] = {0, 1, 1, 1, 1, 1}, four[6] = {0, 1, 2, 3, 1, 2}, five[6] = {0, 1, 2, 3, 4, 1};
    for (uint32_t cpu = 0; cpu < 6; ++cpu) {
        require(sfr::host_processor_slot(cpu, 2) == two[cpu]);
        require(sfr::host_processor_slot(cpu, 4) == four[cpu]);
        require(sfr::host_processor_slot(cpu, 5) == five[cpu]);
    }
}

// The same through the threads themselves, with the process held to four
// processors as on an i5-3470: the main thread's pin and the workers' preferred
// processors never meet, while the workers may still run anywhere allowed.
void four_host_processors_keep_the_main_threads_core_apart() {
    const auto allowed = allowed_process_mask();
    const auto full = sfr::host_processor_order(allowed, 0);
    if (full.size() < 4) return;
    GROUP_AFFINITY original{};
    require(GetThreadGroupAffinity(GetCurrentThread(), &original) != FALSE);
    struct Restore {
        uint64_t process;
        GROUP_AFFINITY thread;
        ~Restore() {
            if (!SetProcessAffinityMask(GetCurrentProcess(), DWORD_PTR(process)) ||
                !SetThreadGroupAffinity(GetCurrentThread(), &thread, nullptr)) std::terminate();
        }
    } restore{allowed, original};
    uint64_t limited = 0;
    for (size_t i = 0; i < 4; ++i) limited |= uint64_t{1} << full[i];
    require(SetProcessAffinityMask(GetCurrentProcess(), DWORD_PTR(limited)) != FALSE);
    auto thread_mask = original;
    thread_mask.Mask = limited;
    require(SetThreadGroupAffinity(GetCurrentThread(), &thread_mask, nullptr) != FALSE);
    const auto order = sfr::host_processor_order(limited, 0);
    require(order.size() == 4);
    const uint64_t main_core = sfr::pin_current_guest_processor(0);
    require(main_core == uint64_t{1} << order[0]);
    sfr::NativeThread worker([](std::stop_token) { return 0; });
    for (uint32_t cpu = 1; cpu < 6; ++cpu) {
        require(worker.set_guest_processor(cpu, true) == limited);
        PROCESSOR_NUMBER ideal{};
        require(GetThreadIdealProcessorEx(worker.native_handle(), &ideal) != FALSE);
        require(ideal.Number != order[0]);
        require(worker.set_guest_processor(cpu) != main_core);
    }
}

void parked_cancel_joins_without_running_entry_and_keeps_handle_open() {
    const DWORD baseline = process_handle_count();
    std::atomic<uint32_t> calls = 0;
    {
        sfr::NativeThread thread([&](std::stop_token) { ++calls; return 0; });
        const uint32_t id = thread.native_id();
        require(process_handle_count() == baseline + 1);

        thread.cancel_and_join();

        require(calls.load() == 0);
        require(!thread.entry_started());
        require(process_handle_count() == baseline + 1);
        require(queried_thread_affinity(id) == thread.affinity_mask());
    }
    require(process_handle_count() == baseline);
}

void active_cancel_waits_for_cooperative_entry_completion() {
    std::atomic<bool> entered = false;
    std::atomic<bool> stop_seen = false;
    std::atomic<bool> release = false;
    std::atomic<bool> exited = false;
    sfr::NativeThread thread([&](std::stop_token stop) {
        entered.store(true);
        while (!stop.stop_requested()) std::this_thread::yield();
        stop_seen.store(true);
        while (!release.load()) std::this_thread::yield();
        exited.store(true);
        return 0;
    });
    require(thread.resume() == 1);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!entered.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    require(entered.load());

    std::jthread releaser([&] {
        while (!stop_seen.load()) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        release.store(true);
    });
    thread.cancel_and_join();
    require(stop_seen.load());
    require(exited.load());
}

void cancel_and_join_is_idempotent() {
    sfr::NativeThread thread([](std::stop_token) { return 0; });
    thread.cancel_and_join();
    thread.cancel_and_join();
    require(!thread.entry_started());
}

}

int main() {
    try {
        construction_parks_the_host_thread();
        resume_runs_on_the_reported_native_thread_and_join_returns_its_code();
        repeated_resume_reports_the_actual_suspend_count();
        joining_a_never_resumed_thread_is_rejected();
        callback_exceptions_are_rethrown_by_join();
        parked_destruction_cancels_before_entry_and_closes_the_handle();
        active_destruction_requests_stop_and_joins();
        empty_entry_is_rejected();
        priority_changes_are_native_and_return_the_previous_value();
        unsupported_priority_is_rejected_without_mutation();
        host_processor_order_spreads_over_physical_cores();
        calling_guest_thread_uses_allowed_core_mapping();
        explicit_cpu_sets_keep_the_calling_thread_affinity();
        six_guest_processors_map_to_allowed_native_processors();
        migrating_workers_keep_their_guest_processor_preference();
        migrating_workers_respect_small_process_cpu_masks();
        host_processor_slots_leave_the_first_to_guest_processor_zero();
        main_thread_is_pinned_only_with_six_or_more_processors();
        four_host_processors_keep_the_main_threads_core_apart();
        invalid_guest_processor_is_rejected_without_mutation();
        parked_cancel_joins_without_running_entry_and_keeps_handle_open();
        active_cancel_waits_for_cooperative_entry_completion();
        cancel_and_join_is_idempotent();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
