#include "job_lifetime.h"
#include "guest_execution.h"
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {
using Gate = sfr::JobLifetime;
using Kind = Gate::Kind;
using namespace std::chrono_literals;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void empty_deletion_list_does_not_gate_an_ordinary_frame() {
    const uint32_t manager = 0x1000, list = 0x2000;
    uint32_t count = 0;
    auto load = [&](uint32_t address) {
        if (address == manager + 184) return list;
        require(address == list + 4, "deletion predicate must use the original list count");
        return count;
    };
    require(!sfr::has_deferred_job_contexts(manager, load), "empty deletion list preserves main/worker overlap");
    count = 2;
    require(sfr::has_deferred_job_contexts(manager, load), "pending actors require the lifetime barrier");
}
void deletion_waits_for_the_batch_and_a_helper_between_poll_and_callback() {
    Gate gate;
    auto batch = std::make_unique<Gate::Lease>(gate, Kind::batch);
    require(batch->try_acquire(), "first batch starts");
    auto helper = std::make_unique<Gate::Lease>(gate, Kind::helper);
    require(helper->try_acquire(), "helper enters before inspecting its queue");
    Gate::Lease deletion(gate, Kind::deletion);
    require(!deletion.try_acquire(), "actor must survive an outstanding batch");
    batch.reset();
    require(!deletion.try_acquire(), "actor must survive a helper that has not entered its callback yet");
    require(!gate.helpers_idle(), "pre-callback queue inspection is outstanding work");
    helper.reset();
    require(gate.helpers_idle(), "helper exit permits job reset");
    require(deletion.try_acquire(), "deletion starts once all readers finish");
}
void waiting_deletion_blocks_new_batches_but_not_helpers_finishing_the_old_batch() {
    Gate gate;
    Gate::Lease batch(gate, Kind::batch);
    require(batch.try_acquire(), "old batch enters");
    Gate::Lease deletion(gate, Kind::deletion);
    require(!deletion.try_acquire(), "deletion waits for old batch");
    Gate::Lease next(gate, Kind::batch);
    require(!next.try_acquire(), "new batch cannot overtake waiting deletion");
    Gate::Lease first(gate, Kind::helper), second(gate, Kind::helper);
    require(first.try_acquire() && second.try_acquire(), "helpers remain concurrent while deletion waits");
}
void deletion_excludes_late_queue_poll_and_queued_batch_until_removal_flags_are_written() {
    Gate gate;
    auto deletion = std::make_unique<Gate::Lease>(gate, Kind::deletion);
    require(deletion->try_acquire(), "deletion can run without an old batch");
    Gate::Lease late_helper(gate, Kind::helper), queued_batch(gate, Kind::batch);
    require(!late_helper.try_acquire(), "late helper cannot inspect jobs during destruction");
    require(!queued_batch.try_acquire(), "queued batch cannot admit destroyed actors mid-destructor");
    deletion.reset();
    require(queued_batch.try_acquire(), "next batch can remove jobs marked by the original destructor");
    require(late_helper.try_acquire(), "late helper can inspect the new batch");
}
void cancelled_wait_and_exception_release_lifetime_state() {
    Gate gate;
    Gate::Lease batch(gate, Kind::batch);
    require(batch.try_acquire(), "batch enters");
    {
        Gate::Lease cancelled(gate, Kind::deletion);
        require(!cancelled.try_acquire(), "deletion is pending before cancellation");
    }
    Gate::Lease next(gate, Kind::batch);
    require(next.try_acquire(), "cancelled deletion must not leave admission closed");
    struct CallbackStopped {};
    try {
        Gate::Lease helper(gate, Kind::helper);
        require(helper.try_acquire(), "helper enters");
        throw CallbackStopped{};
    } catch (const CallbackStopped&) {}
    require(gate.helpers_idle(), "callback exception releases its helper lease");
}
void deletion_wait_releases_the_guest_permit_needed_by_the_helper() {
    Gate gate;
    sfr::GuestExecution execution, core;
    execution.add_follower(core);
    auto main = execution.enter(1);
    auto main_core = core.enter(1);
    main->set_companion(main_core.get());
    auto helper = std::make_unique<Gate::Lease>(gate, Kind::helper);
    require(helper->try_acquire(), "detached helper has entered its queue");
    std::promise<void> finished;
    auto ready = finished.get_future();
    std::jthread worker([&] {
        try {
            auto permit = execution.enter(36);
            auto on_core = core.enter(36);
            helper.reset(); // the accessor and callback can now finish
            finished.set_value();
        } catch (...) { finished.set_exception(std::current_exception()); }
    });
    struct StopOnExit {
        sfr::GuestExecution& execution;
        ~StopOnExit() { execution.stop(); }
    } cleanup{execution}; // release a blocked worker even if an assertion fails
    Gate::Lease deletion(gate, Kind::deletion);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!deletion.try_acquire()) {
        require(std::chrono::steady_clock::now() < deadline, "lifetime wait deadlocked the guest permit");
        main->run_wait([&](std::stop_token) { gate.wait_for_change(); });
    }
    require(ready.wait_for(2s) == std::future_status::ready, "helper exits after releasing the actor lifetime");
    ready.get();
}
}
int main() {
    try {
        empty_deletion_list_does_not_gate_an_ordinary_frame();
        deletion_waits_for_the_batch_and_a_helper_between_poll_and_callback();
        waiting_deletion_blocks_new_batches_but_not_helpers_finishing_the_old_batch();
        deletion_excludes_late_queue_poll_and_queued_batch_until_removal_flags_are_written();
        cancelled_wait_and_exception_release_lifetime_state();
        deletion_wait_releases_the_guest_permit_needed_by_the_helper();
        std::cout << "job lifetime tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
