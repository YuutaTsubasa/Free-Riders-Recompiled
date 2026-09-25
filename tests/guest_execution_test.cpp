#include "guest_execution.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<class F> void cancelled(F operation) {
    try { operation(); }
    catch (const sfr::GuestExecutionCancelled&) { return; }
    throw std::runtime_error("operation was not cancelled");
}

template<class T> T ready(std::future<T>& future, const char* message) {
    require(future.wait_for(2s) == std::future_status::ready, message);
    return future.get();
}

// Long enough for a hold to be worth a measurable number of nanoseconds.
// steady_clock's tick is coarse enough that two adjacent calls can read the
// same instant, and a live hold of zero elapsed nanoseconds is accounted as
// zero -- which is correct, and not what these checks are about.
void hold_a_while() { std::this_thread::sleep_for(2ms); }

struct StopOnExit {
    sfr::GuestExecution& gate;
    ~StopOnExit() { gate.stop(); }
};

void acquires_initial_owner() {
    sfr::GuestExecution gate;
    auto owner = gate.enter(1);
    require(bool(owner), "initial guest acquires execution ownership");
}

void timing_separates_instances_and_accounts_live_holds() {
    sfr::GuestExecution global, core;
    auto owner = global.enter(29);
    hold_a_while();
    const auto first = global.take_timing();
    require(first.held_ns[29] > 0, "snapshot includes a hold before its release");
    require(core.take_timing().held_ns[29] == 0, "another scheduler does not inherit the global hold");
    auto on_core = core.enter(29);
    hold_a_while();
    require(core.take_timing().held_ns[29] > 0, "the core accounts its own live hold");
    hold_a_while();
    owner->detach();
    const auto tail = global.take_timing();
    require(tail.held_ns[29] > 0, "release accounts only the remaining hold interval");
    require(global.take_timing().held_ns[29] == 0, "detached intervals do not count as ownership");
    on_core->detach();
    core.take_timing();
    require(core.take_timing().held_ns[29] == 0, "snapshots consume completed core intervals");
}

void timing_attributes_only_holds_overlapping_main_ready() {
    sfr::GuestExecution gate;
    auto owner = gate.enter(29);
    require(gate.take_timing().main_ready_by_owner_ns[29] == 0,
            "ownership alone is not a main-thread blocker");
    auto main = std::async(std::launch::async, [&] { auto lease = gate.enter(1); });
    StopOnExit cleanup{gate};
    gate.wait_until_ready(1);
    const auto timing = gate.take_timing();
    require(timing.main_ready_by_owner_ns[29] > 0, "live owner is charged while guest 1 is queued");
    require(timing.main_ready_by_owner_ns[29] <= timing.held_ns[29], "overlap cannot exceed ownership");
    owner.reset();
    ready(main, "main gets its turn after the owner releases");
    gate.take_timing();
    require(gate.take_timing().main_ready_by_owner_ns[29] == 0, "completed ready interval is not counted again");
}

void detached_wait_does_not_queue_for_global() {
    sfr::GuestExecution global, core;
    global.add_follower(core);
    auto worker = global.enter(29);
    worker->detach();
    auto on_core = core.enter(29);
    worker->set_companion(on_core.get());
    std::promise<void> global_held, release_global;
    auto release = release_global.get_future();
    auto holder = std::async(std::launch::async, [&] {
        auto lease = global.enter(1);
        global_held.set_value();
        release.wait();
    });
    struct Release { std::promise<void>& signal; ~Release() { signal.set_value(); } } cleanup{release_global};
    global_held.get_future().wait();
    worker->run_wait([&](std::stop_token stop) {
        require(!stop.stop_requested(), "detached wait starts without cancellation");
        require(core.standing().owner == 0, "waiting releases the core");
        require(global.standing().owner == 1 && global.standing().ready.empty(),
                "waiting never queues behind the global owner");
        auto peer = std::async(std::launch::async, [&] { auto lease = core.enter(28); });
        ready(peer, "same-core peer progresses during detached wait");
    });
    require(worker->detached() && !on_core->detached(), "wait restores only core ownership");
    require(global.standing().owner == 1, "global ownership is undisturbed");
}

void detached_wait_observes_cancellation() {
    sfr::GuestExecution global, core;
    global.add_follower(core);
    auto worker = global.enter(29);
    worker->detach();
    auto on_core = core.enter(29);
    worker->set_companion(on_core.get());
    cancelled([&] { worker->run_wait([&](std::stop_token stop) {
        global.stop();
        require(stop.stop_requested(), "detached wait gets the propagated stop token");
    }); });
    require(core.standing().owner == 0, "cancelled wait does not reacquire a stopped core");
}

void blocking_operation_returns_with_ownership() {
    sfr::GuestExecution gate;
    auto owner = gate.enter(1);
    bool called = false;
    owner->run_blocking([&](std::stop_token) { called = true; });
    require(called, "blocking callback runs");
    owner->checkpoint(false);
}

void blocking_operation_releases_permit_for_peer() {
    sfr::GuestExecution gate;
    auto owner = gate.enter(1);
    std::promise<void> callback_started;
    std::promise<void> peer_finished;
    bool guest_value = false;
    auto peer = std::async(std::launch::async, [&] {
        callback_started.get_future().wait();
        auto lease = gate.enter(2);
        guest_value = true;
        peer_finished.set_value();
    });
    StopOnExit cleanup{gate};
    owner->run_blocking([&](std::stop_token) {
        callback_started.set_value();
        peer_finished.get_future().wait();
    });
    require(guest_value, "peer executes while the owner performs native blocking");
    owner->checkpoint(false);
    ready(peer, "peer exits before blocking owner resumes");
}

void blocking_exception_is_rethrown_after_fifo_reacquire() {
    sfr::GuestExecution gate;
    auto owner = gate.enter(1);
    std::atomic<bool> peer_ran = false;
    auto peer = std::async(std::launch::async, [&] {
        auto lease = gate.enter(2);
        peer_ran = true;
    });
    StopOnExit cleanup{gate};
    gate.wait_until_ready(2);
    bool caught = false;
    try {
        owner->run_blocking([&](std::stop_token) {
            throw std::runtime_error("blocking failure");
        });
    } catch (const std::runtime_error& error) {
        caught = std::string(error.what()) == "blocking failure";
        require(peer_ran, "queued peer runs before the failed callback reacquires FIFO ownership");
    }
    require(caught, "blocking callback exception is rethrown");
    owner->checkpoint(false);
    ready(peer, "peer completes around failed blocking callback");
}

void blocked_identity_rejects_duplicates_and_reentry() {
    sfr::GuestExecution gate;
    auto owner = gate.enter(1);
    bool same_id = false;
    bool same_host = false;
    owner->run_blocking([&](std::stop_token) {
        auto duplicate = std::async(std::launch::async, [&] {
            try { (void)gate.enter(1); } catch (const std::logic_error&) { return true; }
            return false;
        });
        same_id = ready(duplicate, "blocked guest ID duplicate rejects promptly");
        try { (void)gate.enter(2); } catch (const std::logic_error&) { same_host = true; }
    });
    require(same_id, "blocked guest ID remains registered");
    require(same_host, "blocked callback host thread cannot reenter under another ID");
}

void stop_interrupts_blocking_operation_without_mutex_deadlock() {
    sfr::GuestExecution gate;
    std::promise<void> callback_started;
    std::atomic<bool> callback_observed_stop = false;
    auto worker = std::async(std::launch::async, [&] {
        auto lease = gate.enter(1);
        try {
            lease->run_blocking([&](std::stop_token token) {
                std::promise<void> interrupted;
                auto interrupted_future = interrupted.get_future();
                std::stop_callback callback(token, [&] {
                    callback_observed_stop = gate.stopped();
                    interrupted.set_value();
                });
                callback_started.set_value();
                interrupted_future.wait();
            });
        } catch (const sfr::GuestExecutionCancelled&) { return true; }
        return false;
    });
    callback_started.get_future().wait();
    gate.stop();
    require(ready(worker, "stop token promptly interrupts native blocking"),
            "stopped blocking owner receives cancellation");
    require(callback_observed_stop, "stop callback can query scheduler state without deadlocking");
}

void cancellation_preserves_blocking_callback_failure() {
    sfr::GuestExecution gate;
    std::promise<void> callback_started;
    auto worker = std::async(std::launch::async, [&] {
        auto lease = gate.enter(1);
        try {
            lease->run_blocking([&](std::stop_token token) {
                std::promise<void> interrupted;
                auto interrupted_future = interrupted.get_future();
                std::stop_callback callback(token, [&] { interrupted.set_value(); });
                callback_started.set_value();
                interrupted_future.wait();
                throw std::runtime_error("native wait failed");
            });
        } catch (const std::runtime_error& error) {
            return std::string(error.what()) == "native wait failed";
        }
        return false;
    });
    callback_started.get_future().wait();
    gate.stop();
    require(ready(worker, "callback failure wins over cancellation"), "actual native wait failure is rethrown");
    try { gate.rethrow_failure(); }
    catch (const std::runtime_error& error) {
        require(std::string(error.what()) == "native wait failed", "callback failure is retained as scheduler cause");
        return;
    }
    throw std::runtime_error("cancelled callback failure was lost");
}

void serializes_real_threads_and_releases_on_unwind() {
    sfr::GuestExecution gate;
    int protected_value = 0;
    std::atomic<int> inside = 0;
    std::atomic<bool> overlap = false;
    std::vector<std::thread> workers;
    for (uint32_t id = 1; id <= 4; ++id) {
        workers.emplace_back([&, id] {
            auto lease = gate.enter(id);
            for (int i = 0; i < 100; ++i) {
                if (inside.fetch_add(1) != 0) overlap = true;
                const int next = protected_value + 1;
                std::this_thread::yield();
                protected_value = next;
                inside.fetch_sub(1);
                lease->checkpoint();
            }
        });
    }
    StopOnExit cleanup{gate};
    for (auto& worker : workers) worker.join();
    require(!overlap && protected_value == 400, "one native thread executes guest work at a time");

    try {
        auto lease = gate.enter(5);
        throw std::runtime_error("unwind");
    } catch (const std::runtime_error&) {}
    auto acquired = std::async(std::launch::async, [&] { return bool(gate.enter(6)); });
    require(ready(acquired, "lease destruction must release the owner"), "next owner acquires after unwind");
}

void hands_off_in_fifo_order() {
    sfr::GuestExecution gate;
    auto owner = gate.enter(1);
    std::mutex order_mutex;
    std::vector<uint32_t> order;
    std::promise<void> release_two, release_three;
    auto two = std::async(std::launch::async, [&] {
        auto lease = gate.enter(2);
        { std::lock_guard lock(order_mutex); order.push_back(2); }
        release_two.get_future().wait();
    });
    StopOnExit cleanup_two{gate};
    gate.wait_until_ready(2);
    auto three = std::async(std::launch::async, [&] {
        auto lease = gate.enter(3);
        { std::lock_guard lock(order_mutex); order.push_back(3); }
        release_three.get_future().wait();
    });
    StopOnExit cleanup_three{gate};
    gate.wait_until_ready(3);
    owner.reset();
    require(two.wait_for(2s) == std::future_status::ready || [&] {
        std::lock_guard lock(order_mutex); return order.size() == 1 && order[0] == 2;
    }(), "first queued guest acquires first");
    release_two.set_value();
    ready(two, "first queued guest exits");
    release_three.set_value();
    ready(three, "second queued guest exits");
    require(order == std::vector<uint32_t>({2, 3}), "ready queue is FIFO");
}

void urgent_guest_preempts_ordinary_owner() {
    sfr::GuestExecution gate;
    gate.set_scheduling_quantum(std::chrono::hours(1));
    gate.set_urgent(3, true);
    auto owner = gate.enter(1);
    std::mutex order_mutex;
    std::vector<uint32_t> order;
    auto two = std::async(std::launch::async, [&] {
        auto lease = gate.enter(2);
        std::lock_guard lock(order_mutex); order.push_back(2);
    });
    StopOnExit cleanup_two{gate};
    gate.wait_until_ready(2);
    auto three = std::async(std::launch::async, [&] {
        auto lease = gate.enter(3);
        std::lock_guard lock(order_mutex); order.push_back(3);
    });
    StopOnExit cleanup_three{gate};
    gate.wait_until_ready(3);
    // The quantum has not expired, but a time-critical guest is waiting.
    for (int i = 0; i < 64; ++i) owner->checkpoint();
    ready(three, "time-critical guest runs");
    ready(two, "ordinary guest runs");
    require(order == std::vector<uint32_t>({3, 2}), "time-critical guest is queued ahead of ordinary guests");
}

void ready_wait_names_a_specific_worker() {
    sfr::GuestExecution gate;
    auto owner = gate.enter(1);
    auto three = std::async(std::launch::async, [&] {
        try { (void)gate.enter(3); }
        catch (const sfr::GuestExecutionCancelled&) { return; }
    });
    StopOnExit cleanup_three{gate};
    gate.wait_until_ready(3);

    std::atomic<bool> two_started = false;
    std::thread starter([&] {
        std::this_thread::sleep_for(50ms);
        two_started = true;
        try { (void)gate.enter(2); }
        catch (const sfr::GuestExecutionCancelled&) {}
    });
    StopOnExit cleanup_starter{gate};
    gate.wait_until_ready(2);
    require(two_started, "an unrelated queued worker does not satisfy a targeted ready wait");
    gate.stop();
    starter.join();
    three.get();
}

void repeated_yields_are_fair() {
    sfr::GuestExecution gate;
    constexpr int rounds = 40;
    std::atomic<int> first_count = 0, second_count = 0;
    std::promise<void> first_has_owner;
    auto first = std::async(std::launch::async, [&] {
        auto lease = gate.enter(1);
        first_has_owner.set_value();
        gate.wait_until_ready(2);
        for (int i = 0; i < rounds; ++i) { ++first_count; lease->checkpoint(); }
    });
    first_has_owner.get_future().wait();
    auto second = std::async(std::launch::async, [&] {
        auto lease = gate.enter(2);
        for (int i = 0; i < rounds; ++i) { ++second_count; lease->checkpoint(); }
    });
    StopOnExit cleanup{gate};
    ready(first, "first yielding guest completes");
    ready(second, "second yielding guest completes");
    require(first_count == rounds && second_count == rounds, "both queued guests make repeated progress");
}

void reservation_blocks_handoff_but_not_cancellation() {
    sfr::GuestExecution gate;
    auto owner = gate.enter(1);
    std::promise<void> acquired;
    auto peer = std::async(std::launch::async, [&] {
        auto lease = gate.enter(2);
        acquired.set_value();
    });
    StopOnExit cleanup{gate};
    gate.wait_until_ready(2);
    owner->checkpoint(false);
    require(acquired.get_future().wait_for(50ms) == std::future_status::timeout,
            "reservation checkpoint retains execution ownership");
    gate.stop();
    cancelled([&] { owner->checkpoint(false); });
    cancelled([&] { peer.get(); });
}

void stop_wakes_entry_and_ready_waiters() {
    sfr::GuestExecution gate;
    auto owner = gate.enter(1);
    auto queued = std::async(std::launch::async, [&] {
        try { (void)gate.enter(2); } catch (const sfr::GuestExecutionCancelled&) { return true; }
        return false;
    });
    gate.wait_until_ready(2);
    auto waiting_ready = std::async(std::launch::async, [&] {
        try { (void)gate.enter(3); } catch (const sfr::GuestExecutionCancelled&) { return true; }
        return false;
    });
    gate.stop();
    require(ready(queued, "stop wakes queued enter"), "queued enter receives cancellation");
    require(ready(waiting_ready, "stop wakes all queued enters"), "all queued entries receive cancellation");
    cancelled([&] { gate.wait_until_ready(4); });
}

void stop_and_drain_waits_for_active_owner() {
    sfr::GuestExecution gate;
    std::promise<void> owner_acquired;
    std::promise<void> release_owner;
    auto owner = std::async(std::launch::async, [&] {
        auto lease = gate.enter(1);
        owner_acquired.set_value();
        release_owner.get_future().wait();
    });
    owner_acquired.get_future().wait();
    auto queued = std::async(std::launch::async, [&] {
        try { (void)gate.enter(2); } catch (const sfr::GuestExecutionCancelled&) { return true; }
        return false;
    });
    auto drainer = std::async(std::launch::async, [&] { gate.stop_and_drain(); });
    const bool queued_cancelled = ready(queued, "drain cancels queued workers");
    const bool returned_early = drainer.wait_for(50ms) == std::future_status::ready;
    release_owner.set_value();
    ready(owner, "active owner releases during drain");
    ready(drainer, "drain returns after active owner releases");
    require(queued_cancelled, "drain rejects future queued admission");
    require(!returned_early, "drain waits until no guest execution owner remains");
}

void preserves_and_rethrows_first_failure() {
    sfr::GuestExecution gate;
    auto owner = gate.enter(1);
    auto queued = std::async(std::launch::async, [&] {
        try { (void)gate.enter(2); } catch (const sfr::GuestExecutionCancelled&) { return true; }
        return false;
    });
    gate.wait_until_ready(2);
    gate.fail(std::make_exception_ptr(std::runtime_error("first")));
    gate.fail(std::make_exception_ptr(std::runtime_error("second")));
    gate.stop();
    require(gate.stopped() && ready(queued, "failure wakes queued enter"), "failure stops scheduler");
    try { gate.rethrow_failure(); }
    catch (const std::runtime_error& error) {
        require(std::string(error.what()) == "first", "first failure is preserved");
        return;
    }
    throw std::runtime_error("stored failure was not rethrown");
}

void records_failure_after_stop_request() {
    sfr::GuestExecution gate;
    gate.stop();
    gate.fail(std::make_exception_ptr(std::runtime_error("in flight")));
    try { gate.rethrow_failure(); }
    catch (const std::runtime_error& error) {
        require(std::string(error.what()) == "in flight", "real failure survives an earlier stop request");
        return;
    }
    throw std::runtime_error("failure after stop request was not retained");
}

void validates_ids_duplicates_and_host_thread() {
    sfr::GuestExecution gate;
    bool zero_rejected = false;
    try { (void)gate.enter(0); } catch (const std::logic_error&) { zero_rejected = true; }
    require(zero_rejected, "zero guest ID is invalid");
    auto owner = gate.enter(1);
    bool empty_blocking_rejected = false;
    try { owner->run_blocking({}); } catch (const std::logic_error&) { empty_blocking_rejected = true; }
    require(empty_blocking_rejected, "blocking operation must be nonempty");
    bool duplicate_rejected = false;
    try { (void)gate.enter(1); } catch (const std::logic_error&) { duplicate_rejected = true; }
    require(duplicate_rejected, "active guest ID cannot enter twice");
    auto wrong_thread = std::async(std::launch::async, [&] {
        try { owner->checkpoint(); } catch (const std::logic_error&) { return true; }
        return false;
    });
    require(ready(wrong_thread, "wrong-host check returns"), "lease is bound to its creating host thread");
    auto wrong_blocking_thread = std::async(std::launch::async, [&] {
        try { owner->run_blocking([](std::stop_token) {}); } catch (const std::logic_error&) { return true; }
        return false;
    });
    require(ready(wrong_blocking_thread, "wrong-host blocking check returns"),
            "blocking operation is bound to the lease host thread");
    bool wrong_owner = false;
    try { gate.wait_until_ready(1); } catch (const std::logic_error&) { wrong_owner = true; }
    require(wrong_owner, "ready wait target cannot be the current owner ID");
    bool reentrant_rejected = false;
    try { (void)gate.enter(2); } catch (const std::logic_error&) { reentrant_rejected = true; }
    require(reentrant_rejected, "owning host thread cannot reenter under another guest ID");
}

void detached_owner_runs_beside_next_owner() {
    sfr::GuestExecution gate;
    auto worker = gate.enter(29);
    worker->detach();
    require(worker->detached(), "detach is visible");
    worker->checkpoint();  // detached checkpoints only observe cancellation
    // The permit is free: another guest enters while the detached one runs.
    std::atomic<bool> main_ran{false};
    auto main = std::async(std::launch::async, [&] {
        auto lease = gate.enter(1);
        main_ran = true;
        lease->checkpoint();
        return true;
    });
    require(ready(main, "main guest enters beside the detached worker"), "main guest ran");
    require(main_ran, "main guest ran while worker detached");
    // The identity stays active while detached.
    bool duplicate = false;
    try { auto again = gate.enter(29); }
    catch (const std::logic_error&) { duplicate = true; }
    require(duplicate, "detached identity cannot enter twice");
    worker->attach();
    require(!worker->detached(), "attach reacquires");
    worker->checkpoint(false);
    bool twice = false;
    try { worker->attach(); } catch (const std::logic_error&) { twice = true; }
    require(twice, "attach without detach is rejected");
}

void attach_waits_for_current_owner() {
    sfr::GuestExecution gate;
    std::promise<void> detached, go;
    std::atomic<bool> attached{false};
    auto worker = std::async(std::launch::async, [&] {
        auto lease = gate.enter(29);
        lease->detach();
        detached.set_value();
        go.get_future().wait();
        lease->attach();
        attached = true;
        lease->checkpoint(false);
        return true;
    });
    detached.get_future().wait();
    auto main = gate.enter(1);
    go.set_value();
    require(worker.wait_for(100ms) == std::future_status::timeout, "attach waits while main owns the permit");
    require(!attached, "no second owner while main runs");
    main.reset();
    require(ready(worker, "attach proceeds after main releases"), "attached");
}

void companion_is_released_while_waiting() {
    sfr::GuestExecution global, core;
    auto owner = global.enter(1);
    auto on_core = core.enter(1);
    owner->set_companion(on_core.get());
    bool after = false;
    owner->set_after_blocking([&] { after = true; });
    // While the owner blocks, another thread can take the core.
    owner->run_blocking([&](std::stop_token) {
        require(on_core->detached(), "companion released during the blocking operation");
        auto other = std::async(std::launch::async, [&] { auto lease = core.enter(2); return true; });
        require(ready(other, "core free while its holder blocks"), "entered core");
    });
    require(!on_core->detached() && after, "companion taken back after the permit, then the hook runs");
    on_core->checkpoint(false);
    owner->checkpoint(false);
    // attach() releases the companion while it waits for the permit.
    owner->detach();
    std::promise<void> held;
    std::thread holder([&] {
        auto lease = global.enter(3);
        held.set_value();
        std::this_thread::sleep_for(100ms);
    });
    held.get_future().wait();
    auto worker = std::async(std::launch::async, [&] { auto lease = core.enter(4); return true; });
    core.wait_until_ready(4);  // queued behind this thread's core permit
    owner->attach();
    holder.join();
    require(ready(worker, "core was free while attach waited"), "core entered during attach");
    require(!owner->detached() && !on_core->detached(), "both permits held again after attach");
}

void followers_stop_with_their_leader() {
    sfr::GuestExecution global, core;
    global.add_follower(core);
    auto lease = core.enter(5);
    const auto token = global.stop_token();
    require(!token.stop_requested(), "stop token starts clear");
    global.stop();
    require(token.stop_requested() && core.stopped(), "stopping the leader stops its follower");
    cancelled([&] { lease->checkpoint(); });
}

void stop_cancels_detached_checkpoint() {
    sfr::GuestExecution gate;
    auto worker = gate.enter(29);
    worker->detach();
    gate.stop();
    cancelled([&] { worker->checkpoint(); });
    cancelled([&] { worker->attach(); });
    worker.reset();
}
}

int main() {
    struct Test { const char* name; void (*run)(); };
    for (auto test : {Test{"live per-instance timing", timing_separates_instances_and_accounts_live_holds},
                      {"main-ready attribution", timing_attributes_only_holds_overlapping_main_ready},
                      {"detached wait", detached_wait_does_not_queue_for_global},
                      {"detached wait cancellation", detached_wait_observes_cancellation},
                      {"initial owner", acquires_initial_owner},
                      {"blocking operation ownership", blocking_operation_returns_with_ownership},
                      {"blocking operation permit release", blocking_operation_releases_permit_for_peer},
                      {"blocking exception reacquire", blocking_exception_is_rethrown_after_fifo_reacquire},
                      {"blocked identity", blocked_identity_rejects_duplicates_and_reentry},
                      {"blocking stop token", stop_interrupts_blocking_operation_without_mutex_deadlock},
                      {"blocking cancellation failure", cancellation_preserves_blocking_callback_failure},
                      {"serialization and unwind", serializes_real_threads_and_releases_on_unwind},
                      {"FIFO handoff", hands_off_in_fifo_order},
                      {"time-critical preemption", urgent_guest_preempts_ordinary_owner},
                      {"targeted ready wait", ready_wait_names_a_specific_worker},
                      {"repeated yield fairness", repeated_yields_are_fair},
                      {"reservation and cancellation", reservation_blocks_handoff_but_not_cancellation},
                      {"stop wakes waiters", stop_wakes_entry_and_ready_waiters},
                      {"stop and drain", stop_and_drain_waits_for_active_owner},
                      {"first failure", preserves_and_rethrows_first_failure},
                      {"failure after stop", records_failure_after_stop_request},
                      {"validation", validates_ids_duplicates_and_host_thread},
                      {"detached owner", detached_owner_runs_beside_next_owner},
                      {"attach waits", attach_waits_for_current_owner},
                      {"detached cancellation", stop_cancels_detached_checkpoint},
                      {"companion permit", companion_is_released_while_waiting},
                      {"stop followers", followers_stop_with_their_leader}}) {
        try { test.run(); }
        catch (const std::exception& error) {
            std::cerr << test.name << ": " << error.what() << '\n';
            return 1;
        }
    }
    std::cout << "Guest execution checks passed (18 groups)\n";
}
