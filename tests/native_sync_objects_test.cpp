#include "native_sync_objects.h"

#include "test_platform.h"

#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <memory>
#include <stop_token>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {
constexpr uint32_t status_success = 0;
constexpr uint32_t status_timeout = 0x102;
constexpr uint32_t status_invalid_handle = 0xc0000008u;
constexpr uint32_t status_invalid_parameter = 0xc000000du;
constexpr uint32_t status_semaphore_limit_exceeded = 0xc0000047u;
constexpr uint32_t status_object_type_mismatch = 0xc0000024u;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

DWORD process_handle_count();

void retaining_an_invalid_handle_fails() {
    sfr::NativeSyncObjects objects;
    require(objects.retain_wait(0x72100004u) == nullptr,
            "retaining an unowned guest handle fails");
}

void retained_wait_observes_a_signaled_event() {
    sfr::NativeSyncObjects objects;
    const auto event = objects.create_event(false, true);
    require(event.status == status_success, "signaled retained-wait event creates");
    auto retained = objects.retain_wait(event.handle);
    require(retained != nullptr, "valid guest handle can be retained for waiting");
    const auto result = retained->wait(0);
    require(result.status == status_success && !result.cancelled,
            "retained wait observes the native event signal");
}

void retained_wait_times_out_without_cancellation() {
    sfr::NativeSyncObjects objects;
    const auto event = objects.create_event(false, false);
    auto retained = objects.retain_wait(event.handle);
    const auto result = retained->wait(0);
    require(result.status == status_timeout && !result.cancelled,
            "retained wait reports a genuine native timeout");
}

void retained_wait_consumes_auto_event_and_semaphore_state() {
    sfr::NativeSyncObjects objects;
    const auto event = objects.create_event(false, true);
    auto first_event_wait = objects.retain_wait(event.handle);
    auto second_event_wait = objects.retain_wait(event.handle);
    require(first_event_wait->wait(0).status == status_success &&
            second_event_wait->wait(0).status == status_timeout,
            "retained waits consume an auto-reset event exactly once");

    const auto semaphore = objects.create_semaphore(2, 2);
    auto semaphore_wait = objects.retain_wait(semaphore.handle);
    require(semaphore_wait->wait(0).status == status_success &&
            semaphore_wait->wait(0).status == status_success &&
            semaphore_wait->wait(0).status == status_timeout,
            "retained waits consume the native semaphore count");
}

void prestopped_wait_has_priority_over_a_signaled_object() {
    sfr::NativeSyncObjects objects;
    const auto event = objects.create_event(false, true);
    auto retained = objects.retain_wait(event.handle);
    std::stop_source cancellation;
    cancellation.request_stop();

    const auto cancelled = retained->wait(INFINITE, cancellation.get_token());
    require(cancelled.status == status_success && cancelled.cancelled,
            "pre-stopped wait reports cancellation even when its target is signaled");
    require(objects.wait(event.handle, 0) == status_success &&
            objects.wait(event.handle, 0) == status_timeout,
            "cancellation priority does not consume the signaled auto-reset event");
}

void cancellation_unblocks_an_infinite_pending_wait() {
    sfr::NativeSyncObjects objects;
    const auto event = objects.create_event(false, false);
    auto retained = objects.retain_wait(event.handle);
    std::stop_source cancellation;
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    auto result_future = std::async(std::launch::async, [&] {
        entered.set_value();
        return retained->wait(INFINITE, cancellation.get_token());
    });
    entered_future.get();
    cancellation.request_stop();
    require(result_future.wait_for(std::chrono::seconds(1)) == std::future_status::ready,
            "cancellation promptly unblocks an infinite retained wait");
    const auto result = result_future.get();
    require(result.status == status_success && result.cancelled,
            "unblocked retained wait identifies cancellation separately");
}

void retained_duplicate_survives_guest_close_and_registry_destruction() {
    std::unique_ptr<sfr::NativeSyncObjects::WaitHandle> retained;
    {
        sfr::NativeSyncObjects objects;
        const auto event = objects.create_event(false, true);
        retained = objects.retain_wait(event.handle);
        require(objects.close(event.handle) == status_success,
                "guest close releases the registry-owned handle");
    }
    const auto result = retained->wait(0);
    require(result.status == status_success && !result.cancelled,
            "retained duplicate remains usable after guest close and registry destruction");
}

void pending_retained_wait_survives_concurrent_guest_close() {
    sfr::NativeSyncObjects objects;
    const auto event = objects.create_event(false, false);
    auto retained = objects.retain_wait(event.handle);
    std::stop_source cancellation;
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    auto result_future = std::async(std::launch::async, [&] {
        entered.set_value();
        return retained->wait(INFINITE, cancellation.get_token());
    });
    entered_future.get();
    require(objects.close(event.handle) == status_success,
            "guest close succeeds while a retained wait is pending");
    cancellation.request_stop();
    require(result_future.wait_for(std::chrono::seconds(1)) == std::future_status::ready,
            "pending retained wait remains cancellable after guest close");
    require(result_future.get().cancelled,
            "pending retained wait reports cancellation after guest close");
}

void retained_waits_do_not_leak_native_handles() {
    {
        sfr::NativeSyncObjects warmup;
        const auto event = warmup.create_event(false, false);
        auto retained = warmup.retain_wait(event.handle);
        require(retained->wait(0).status == status_timeout, "retained-wait warmup completes");
    }
    const DWORD before = process_handle_count();
    {
        sfr::NativeSyncObjects objects;
        const auto event = objects.create_event(false, false);
        for (int i = 0; i < 8; ++i) {
            auto retained = objects.retain_wait(event.handle);
            require(retained->wait(0).status == status_timeout,
                    "temporary retained wait times out during leak check");
        }
    }
    require(process_handle_count() == before,
            "retained duplicates and per-call cancellation events are all closed");
}

DWORD process_handle_count() {
    DWORD count = 0;
    require(GetProcessHandleCount(GetCurrentProcess(), &count) != FALSE,
            "process handle count is available");
    return count;
}

void uses_real_wait_and_release_state() {
    sfr::NativeSyncObjects semaphores;
    const auto created = semaphores.create_semaphore(1, 1);
    require(created.status == status_success && created.handle == 0x72100004u,
            "first valid semaphore receives the first opaque handle");
    require(semaphores.owns(created.handle) && semaphores.open_count() == 1,
            "created semaphore remains owned");
    require(semaphores.wait(created.handle, 0) == status_success,
            "initial native semaphore count satisfies a wait");
    require(semaphores.wait(created.handle, 0) == status_timeout,
            "depleted native semaphore times out");
    const auto released = semaphores.release_semaphore(created.handle, 1);
    require(released.status == status_success && released.previous_count == 0,
            "native release reports the previous count");
    const auto excessive = semaphores.release_semaphore(created.handle, 1);
    require(excessive.status == status_semaphore_limit_exceeded && excessive.previous_count == 0,
            "over-release reports the native limit error without a fabricated count");
    require(semaphores.wait(created.handle, 0) == status_success &&
            semaphores.wait(created.handle, 0) == status_timeout,
            "failed over-release leaves the native count unchanged");
}

void supports_multi_count_semaphores() {
    sfr::NativeSyncObjects semaphores;
    const auto created = semaphores.create_semaphore(2, 3);
    require(created.status == status_success, "multi-count semaphore creates");
    require(semaphores.wait(created.handle, 0) == status_success &&
            semaphores.wait(created.handle, 0) == status_success &&
            semaphores.wait(created.handle, 0) == status_timeout,
            "two initial permits are consumed by native waits");
    const auto released = semaphores.release_semaphore(created.handle, 3);
    require(released.status == status_success && released.previous_count == 0,
            "multi-release reports native previous count");
    for (int i = 0; i < 3; ++i)
        require(semaphores.wait(created.handle, 0) == status_success,
                "each released permit can be consumed");
    require(semaphores.wait(created.handle, 0) == status_timeout,
            "all released permits are consumed");
}

void validates_create_and_release_parameters() {
    sfr::NativeSyncObjects semaphores;
    for (const auto values : {std::pair{-1, 1}, std::pair{0, 0},
                              std::pair{2, 1}, std::pair{0, -1}}) {
        const auto result = semaphores.create_semaphore(values.first, values.second);
        require(result.status == status_invalid_parameter && result.handle == 0,
                "invalid create counts return STATUS_INVALID_PARAMETER");
    }
    require(semaphores.open_count() == 0, "invalid creates allocate no handles");
    const auto created = semaphores.create_semaphore(0, 2);
    require(created.status == status_success, "zero initial count is valid");
    for (int32_t adjustment : {0, -1}) {
        const auto result = semaphores.release_semaphore(created.handle, adjustment);
        require(result.status == status_invalid_parameter && result.previous_count == 0,
                "non-positive adjustment returns STATUS_INVALID_PARAMETER");
    }
    require(semaphores.wait(created.handle, 0) == status_timeout,
            "invalid releases preserve native count");
}

void closes_handles_and_never_reuses_ids() {
    sfr::NativeSyncObjects semaphores;
    const auto first = semaphores.create_semaphore(0, 1);
    const auto second = semaphores.create_semaphore(0, 1);
    require(first.status == 0 && second.status == 0 && second.handle == first.handle + 4,
            "guest semaphore IDs increase by four");
    require(semaphores.close(first.handle) == status_success && !semaphores.owns(first.handle) &&
            semaphores.open_count() == 1, "close releases ownership exactly once");
    require(semaphores.close(first.handle) == status_invalid_handle &&
            semaphores.wait(first.handle, 0) == status_invalid_handle &&
            semaphores.release_semaphore(first.handle, 1).status == status_invalid_handle,
            "stale handles are invalid for every operation");
    const auto third = semaphores.create_semaphore(0, 1);
    require(third.status == 0 && third.handle == second.handle + 4,
            "closed guest IDs are never reused");
}

void rejects_foreign_and_namespace_handles() {
    sfr::NativeSyncObjects semaphores;
    for (uint32_t handle : {0u, 0x720ffffcu, 0x72100000u, 0x72100004u,
                            0x721ffffcu, 0x72200000u, 0xffffffffu}) {
        require(semaphores.wait(handle, 0) == status_invalid_handle,
                "foreign wait handle is invalid");
        require(semaphores.release_semaphore(handle, 1).status == status_invalid_handle,
                "foreign release handle is invalid");
        require(semaphores.set_event(handle) == status_invalid_handle &&
                semaphores.reset_event(handle) == status_invalid_handle,
                "foreign event-operation handle is invalid");
        require(semaphores.close(handle) == status_invalid_handle && !semaphores.owns(handle),
                "foreign close handle is invalid");
    }
    require(!sfr::NativeSyncObjects::is_handle_range(0x720fffffu) &&
            sfr::NativeSyncObjects::is_handle_range(0x72100000u) &&
            sfr::NativeSyncObjects::is_handle_range(0x721fffffu) &&
            !sfr::NativeSyncObjects::is_handle_range(0x72200000u),
            "namespace range uses exact half-open bounds");
}

void auto_reset_events_release_one_wait() {
    sfr::NativeSyncObjects objects;
    const auto event = objects.create_event(false, false);
    require(event.status == status_success && event.handle == 0x72100004u,
            "auto-reset event receives the first shared sync-object handle");
    require(objects.wait(event.handle, 0) == status_timeout, "unsignaled auto-reset event times out");
    require(objects.set_event(event.handle) == status_success, "auto-reset event can be signaled");
    require(objects.wait(event.handle, 0) == status_success &&
            objects.wait(event.handle, 0) == status_timeout,
            "auto-reset event releases exactly one native wait");

    const auto initially_set = objects.create_event(false, true);
    require(initially_set.status == status_success && objects.wait(initially_set.handle, 0) == status_success &&
            objects.wait(initially_set.handle, 0) == status_timeout,
            "initially signaled auto-reset event is consumed once");
}

void manual_reset_events_remain_signaled_until_reset() {
    sfr::NativeSyncObjects objects;
    const auto event = objects.create_event(true, true);
    require(event.status == status_success, "manual-reset event creates signaled");
    require(objects.wait(event.handle, 0) == status_success &&
            objects.wait(event.handle, 0) == status_success,
            "manual-reset event remains signaled across waits");
    require(objects.reset_event(event.handle) == status_success &&
            objects.wait(event.handle, 0) == status_timeout,
            "reset clears a manual-reset event");
    require(objects.set_event(event.handle) == status_success &&
            objects.wait(event.handle, 0) == status_success,
            "manual-reset event can be signaled again");
}

void rejects_cross_kind_operations_without_state_changes() {
    sfr::NativeSyncObjects objects;
    const auto semaphore = objects.create_semaphore(0, 1);
    const auto event = objects.create_event(false, false);
    require(semaphore.status == 0 && event.status == 0 && event.handle == semaphore.handle + 4,
            "events and semaphores share one monotonic handle namespace");
    require(objects.set_event(semaphore.handle) == status_object_type_mismatch &&
            objects.reset_event(semaphore.handle) == status_object_type_mismatch,
            "event operations reject semaphore handles");
    require(objects.wait(semaphore.handle, 0) == status_timeout,
            "cross-kind event operations preserve semaphore count");
    require(objects.release_semaphore(event.handle, 1).status == status_object_type_mismatch,
            "semaphore release rejects an event handle");
    require(objects.wait(event.handle, 0) == status_timeout,
            "cross-kind semaphore release preserves event state");
}

void destructor_closes_remaining_native_handles() {
    {
        sfr::NativeSyncObjects warmup;
        require(warmup.create_semaphore(0, 1).status == 0 &&
                warmup.create_event(false, false).status == 0, "warmup native sync creation");
    }
    const DWORD before = process_handle_count();
    {
        sfr::NativeSyncObjects semaphores;
        for (int i = 0; i < 4; ++i) {
            require(semaphores.create_semaphore(0, 1).status == 0,
                    "destructor fixture creates native semaphores");
            require(semaphores.create_event(i % 2 == 0, false).status == 0,
                    "destructor fixture creates native events");
        }
#ifdef _WIN32  // the portable objects hold no descriptors
        require(process_handle_count() >= before + 8, "each open semaphore owns a native handle");
#endif
    }
    require(process_handle_count() == before, "destructor closes every remaining native handle");
}
}

int main() {
    try {
        retaining_an_invalid_handle_fails();
        retained_wait_observes_a_signaled_event();
        retained_wait_times_out_without_cancellation();
        retained_wait_consumes_auto_event_and_semaphore_state();
        prestopped_wait_has_priority_over_a_signaled_object();
        cancellation_unblocks_an_infinite_pending_wait();
        retained_duplicate_survives_guest_close_and_registry_destruction();
        pending_retained_wait_survives_concurrent_guest_close();
        retained_waits_do_not_leak_native_handles();
        uses_real_wait_and_release_state();
        supports_multi_count_semaphores();
        validates_create_and_release_parameters();
        closes_handles_and_never_reuses_ids();
        rejects_foreign_and_namespace_handles();
        auto_reset_events_release_one_wait();
        manual_reset_events_remain_signaled_until_reset();
        rejects_cross_kind_operations_without_state_changes();
        destructor_closes_remaining_native_handles();
        std::cout << "Native sync object checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
