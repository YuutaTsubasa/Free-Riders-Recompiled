#include "guest_execution.h"
#include "guest_memory.h"
#include "native_sync_objects.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>
#include "test_platform.h"
#include <malloc.h>

namespace admission_allocation_failure {
thread_local int countdown = -1;
void check() {
    if (countdown >= 0 && countdown-- == 0) {
        countdown = -1;
        throw std::bad_alloc();
    }
}
struct Scope {
    explicit Scope(int index) { countdown = index; }
    ~Scope() { countdown = -1; }
};
}
void* operator new(std::size_t size) {
    admission_allocation_failure::check();
    if (void* result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void* operator new(std::size_t size, std::align_val_t alignment) {
    admission_allocation_failure::check();
    if (void* result = _aligned_malloc(size ? size : 1, static_cast<std::size_t>(alignment))) return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }
void operator delete(void* pointer, std::align_val_t) noexcept { _aligned_free(pointer); }
void operator delete[](void* pointer, std::align_val_t alignment) noexcept { ::operator delete(pointer, alignment); }
void operator delete(void* pointer, std::size_t, std::align_val_t alignment) noexcept { ::operator delete(pointer, alignment); }
void operator delete[](void* pointer, std::size_t, std::align_val_t alignment) noexcept { ::operator delete(pointer, alignment); }

namespace {
using namespace std::chrono_literals;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void rejects(F operation) {
    try { operation(); } catch (const sfr::RuntimeStop&) { return; }
    throw std::runtime_error("invalid retained event did not stop");
}
template<class F> void rejects_nested(F operation) {
    try { operation(); } catch (const std::logic_error&) { return; }
    throw std::runtime_error("same host thread acquired nested execution ownership");
}
template<class T> T ready(std::future<T>& result, const char* message) {
    require(result.wait_for(2s) == std::future_status::ready, message);
    return result.get();
}
struct StopExecution {
    sfr::GuestExecution& execution;
    ~StopExecution() { execution.stop(); }
};

void blocking_admission_allocates_before_effects_and_keeps_owner_on_failure() {
    unsigned failures = 0;
    bool succeeded = false;
    for (int index = 0; index != 16; ++index) {
        sfr::GuestExecution execution;
        auto lease = execution.enter(10);
        bool before = false, native = false;
        // Construct both function objects before arming the allocator.
        std::function<void()> effect = [&] {
            admission_allocation_failure::countdown = -1;
            before = true;
        };
        std::function<void(std::stop_token)> operation = [&](std::stop_token) { native = true; };
        bool failed = false;
        try {
            admission_allocation_failure::Scope injection(index);
            lease->run_blocking(std::move(operation), std::move(effect));
        } catch (const std::bad_alloc&) { failed = true; }
        lease->checkpoint(false);
        if (failed) {
            ++failures;
            require(!before && !native, "admission allocation failure must precede guest effects and native work");
            lease->run_blocking([&](std::stop_token) { native = true; }, [&] { before = true; });
            require(before && native, "failed admission leaves no phantom blocked entry and permits same-ID retry");
        } else {
            require(before && native, "successful admission executes its effect before native work");
            succeeded = true;
            break;
        }
    }
    require(succeeded && failures >= 2, "exercise both waiter and first blocked-entry allocations before effects");
}

void blocking_before_release_exception_rolls_back_admission() {
    struct Expected {};
    sfr::GuestExecution execution;
    auto lease = execution.enter(UINT32_MAX);
    bool native = false, caught = false;
    try {
        lease->run_blocking([&](std::stop_token) { native = true; }, [] { throw Expected{}; });
    } catch (const Expected&) { caught = true; }
    require(caught && !native, "before-release exception propagates without starting native work");
    lease->checkpoint(false);
    unsigned before = 0;
    lease->run_blocking([&](std::stop_token) { native = true; }, [&] { ++before; });
    require(before == 1 && native, "callback rollback retains the permit and permits same-ID blocking retry");
    lease.reset();
    auto again = execution.enter(UINT32_MAX);
    again->checkpoint(false);
}

void blocking_before_release_precedes_exclusive_owner_handoff() {
    sfr::GuestExecution execution;
    auto lease = execution.enter(10);
    std::atomic<bool> before = false, other_ran = false;
    auto other = std::async(std::launch::async, [&] {
        auto guest = execution.enter(11);
        require(before.load(), "queued guest cannot run until the admission effect is complete");
        other_ran = true;
    });
    StopExecution cleanup{execution};
    execution.wait_until_ready(11);
    lease->run_blocking([&](std::stop_token) {
        ready(other, "queued guest runs during native-only phase");
    }, [&] {
        require(!other_ran, "before-release callback still has exclusive guest ownership");
        before = true;
    });
    require(before && other_ran, "callback completes before handoff and native phase releases the permit");
    lease->checkpoint(false);
}

void completion_uses_fifo_ownership_distinct_from_full_width_guest_ids() {
    sfr::GuestExecution execution;
    auto owner = execution.enter(UINT32_MAX);
    std::vector<unsigned> order;
    auto guest = std::async(std::launch::async, [&] {
        auto lease = execution.enter(10);
        order.push_back(1);
    });
    StopExecution cleanup{execution};
    execution.wait_until_ready(10);
    auto first = std::async(std::launch::async, [&] {
        auto lease = execution.enter_completion();
        lease->checkpoint(false);
        order.push_back(2);
    });
    auto second = std::async(std::launch::async, [&] {
        auto lease = execution.enter_completion();
        lease->checkpoint(false);
        order.push_back(3);
    });
    owner.reset();
    ready(guest, "queued guest acquires before later completion entrants");
    ready(first, "first completion acquires without colliding with a guest ID");
    ready(second, "second simultaneous completion receives its own internal identity");
    require(order.size() == 3 && order.front() == 1 && order[1] != order[2],
            "completion entrants use the existing FIFO permit behind an already queued guest");
    auto maximum_guest = execution.enter(UINT32_MAX);
    maximum_guest->checkpoint(false);
}

void completion_owner_yields_and_blocks_with_the_same_fifo_lease_rules() {
    sfr::GuestExecution execution;
    auto completion = execution.enter_completion();
    rejects_nested([&] { (void)execution.enter(10); });
    rejects_nested([&] { (void)execution.enter_completion(); });
    bool guest_ran = false;
    auto guest = std::async(std::launch::async, [&] {
        auto lease = execution.enter(UINT32_MAX);
        guest_ran = true;
    });
    StopExecution cleanup{execution};
    execution.wait_until_ready(UINT32_MAX);
    completion->checkpoint();
    require(guest_ran, "completion checkpoint hands the same FIFO permit to an existing guest");
    ready(guest, "guest completes before completion owner resumes");
    completion->run_blocking([&](std::stop_token) {
        rejects_nested([&] { (void)execution.enter_completion(); });
        rejects_nested([&] { (void)execution.enter(10); });
    });
    completion->checkpoint(false);
}

void completion_can_publish_while_issuing_guest_is_registered_as_blocked() {
    sfr::GuestExecution execution;
    std::promise<void> blocked;
    auto blocked_future = blocked.get_future();
    std::mutex mutex;
    std::condition_variable_any changed;
    bool published = false;
    auto issuer = std::async(std::launch::async, [&] {
        auto lease = execution.enter(10);
        lease->run_blocking([&](std::stop_token stop) {
            std::unique_lock lock(mutex);
            blocked.set_value();
            changed.wait(lock, stop, [&] { return published; });
        });
        lease->checkpoint(false);
        return published;
    });
    StopExecution cleanup{execution};
    ready(blocked_future, "issuing guest reaches host-only blocked state");
    {
        auto completion = execution.enter_completion();
        rejects_nested([&] { (void)execution.enter(10); });
        std::lock_guard lock(mutex);
        published = true;
        changed.notify_all();
    }
    require(ready(issuer, "original guest can resume after independent completion publication"),
            "completion does not impersonate or replace the still-registered issuing guest");
}

void completion_cannot_interrupt_a_guest_reservation_interval() {
    sfr::GuestMemory memory;
    memory.map(0x10000, 4);
    memory.store<uint32_t>(0x10000, 7);
    sfr::GuestExecution execution;
    auto guest = execution.enter(UINT32_MAX);
    const auto reserved = memory.load_reserved_word(0x10000);
    std::atomic<bool> entered = false;
    auto worker = std::async(std::launch::async, [&] {
        auto completion = execution.enter_completion();
        entered = true;
        require(!memory.has_reservation(), "completion publication never enters a live guest atomic interval");
        memory.store<uint32_t>(0x10000, 11);
    });
    StopExecution cleanup{execution};
    guest->checkpoint(!memory.has_reservation());
    require(!entered && worker.wait_for(20ms) == std::future_status::timeout,
            "non-handoff checkpoint retains ownership while a PPC reservation is live");
    require(memory.store_conditional_word(0x10000, reserved + 1),
            "queued completion does not break the original conditional store");
    guest.reset();
    ready(worker, "completion acquires once original guest permits handoff");
    auto verifier = execution.enter(10);
    require(memory.load<uint32_t>(0x10000) == 11, "completion publishes under normal exclusive ownership");
}

void global_stop_denies_completion_ownership_without_publication() {
    sfr::GuestExecution execution;
    auto guest = execution.enter(UINT32_MAX);
    std::atomic<unsigned> publications = 0;
    auto worker = std::async(std::launch::async, [&] {
        try {
            auto completion = execution.enter_completion();
            ++publications;
            return false;
        } catch (const sfr::GuestExecutionCancelled&) { return true; }
    });
    StopExecution cleanup{execution};
    execution.stop();
    guest.reset();
    require(ready(worker, "global stop releases a queued completion waiter") && publications == 0,
            "stopping never grants a completion lease or fabricates publication");
    bool cancelled = false;
    try { (void)execution.enter_completion(); }
    catch (const sfr::GuestExecutionCancelled&) { cancelled = true; }
    require(cancelled, "completion entry after stop is cancellation rather than ownership");
    execution.stop_and_drain();
}

void retained_auto_and_manual_events_survive_guest_close_and_name_retirement() {
    sfr::GuestExecution execution;
    auto lease = execution.enter(10);
    for (bool manual : {false, true}) {
        sfr::NativeSyncObjects objects;
        const auto original = objects.create_event(manual, true, "CompletionEvent");
        auto target = objects.retain_event(original.handle);
        auto waiter = objects.retain_wait(original.handle);
        require(target && waiter, "live event supplies independent reset/signal and wait capabilities");
        require(objects.open_count() == 1, "retained capability does not invent another guest handle");
        target->reset();
        require(waiter->wait(0).status == 0x102, "retained reset changes the actual original event");
        require(objects.close(original.handle) == 0 && !objects.owns(original.handle),
                "guest close retires its handle and name while capabilities remain alive");
        const auto replacement = objects.create_event(manual, false, "CompletionEvent");
        require(replacement.status == 0 && replacement.handle != original.handle,
                "retired name denotes a newly created independent native event");
        target->signal();
        require(waiter->wait(0).status == 0 && waiter->wait(0).status == (manual ? 0u : 0x102u),
                "retained signal preserves original auto/manual semantics after guest close");
        require(objects.wait(replacement.handle, 0) == 0x102,
                "old retained capability does not signal a replacement guest name/ID");
        target->reset();
        require(waiter->wait(0).status == 0x102, "retained reset still works after name retirement");
    }
}

void retained_event_rejects_wrong_kind_and_outlives_registry_without_leaks() {
    sfr::GuestExecution execution;
    auto lease = execution.enter(10);
    DWORD before = 0, after = 0;
    require(GetProcessHandleCount(GetCurrentProcess(), &before), "query initial native handle count");
    {
        std::unique_ptr<sfr::NativeSyncObjects::RetainedEvent> retained;
        std::unique_ptr<sfr::NativeSyncObjects::WaitHandle> waiter;
        {
            sfr::NativeSyncObjects objects;
            for (uint32_t invalid : {0u, UINT32_MAX, 0x72100004u})
                require(!objects.retain_event(invalid), "invalid event retention returns no capability");
            const auto semaphore = objects.create_semaphore(1, 1);
            rejects([&] { (void)objects.retain_event(semaphore.handle); });
            require(objects.open_count() == 1 && objects.wait(semaphore.handle, 0) == 0 &&
                        objects.wait(semaphore.handle, 0) == 0x102,
                    "wrong-kind retention neither creates handles nor consumes semaphore state");
            const auto event = objects.create_event(true, false);
            retained = objects.retain_event(event.handle);
            waiter = objects.retain_wait(event.handle);
            require(retained && waiter, "event capabilities are retained before registry destruction");
        }
        retained->signal();
        require(waiter->wait(0).status == 0, "capability owns the kernel event after registry destruction");
        auto moved = std::move(*retained);
        moved.reset();
        require(waiter->wait(0).status == 0x102, "moving capability transfers reset/signal ownership exactly once");
    }
    require(GetProcessHandleCount(GetCurrentProcess(), &after) && after == before,
            "capability, waiter and registry destruction close every duplicate native handle");
}
}

int main() {
    unsigned failures = 0;
    for (auto test : {blocking_admission_allocates_before_effects_and_keeps_owner_on_failure,
                      blocking_before_release_exception_rolls_back_admission,
                      blocking_before_release_precedes_exclusive_owner_handoff,
                      completion_uses_fifo_ownership_distinct_from_full_width_guest_ids,
                      completion_owner_yields_and_blocks_with_the_same_fifo_lease_rules,
                      completion_can_publish_while_issuing_guest_is_registered_as_blocked,
                      completion_cannot_interrupt_a_guest_reservation_interval,
                      global_stop_denies_completion_ownership_without_publication,
                      retained_auto_and_manual_events_survive_guest_close_and_name_retirement,
                      retained_event_rejects_wrong_kind_and_outlives_registry_without_leaks}) {
        try { test(); }
        catch (const std::exception& error) { std::cerr << error.what() << '\n'; ++failures; }
    }
    return failures ? 1 : 0;
}
