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
    return uint64_t{1} << order[guest_cpu % order.size()];
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
        six_guest_processors_map_to_allowed_native_processors();
        invalid_guest_processor_is_rejected_without_mutation();
        parked_cancel_joins_without_running_entry_and_keeps_handle_open();
        active_cancel_waits_for_cooperative_entry_completion();
        cancel_and_join_is_idempotent();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
