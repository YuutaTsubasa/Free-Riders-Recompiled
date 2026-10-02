#include "diagnostic_hooks.h"
#include "guest_checkpoint_interval.h"
#include "guest_execution.h"
#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

struct PPCContext {};
namespace sfr {
thread_local uint32_t test_checkpoint_interval = 32, test_checkpoint_calls = 0;
thread_local GuestExecution::Lease* test_checkpoint_lease = nullptr;
void guest_checkpoint_permit() {
    guest_thread_state.entry.checkpoint_countdown = test_checkpoint_interval - 1;
    ++test_checkpoint_calls;
    if (test_checkpoint_lease) test_checkpoint_lease->checkpoint();
}
void enter_function_observed(PPCContext&, const char* name, uint32_t address) {
    guest_thread_state.entry.observed = false;
    guest_thread_state.entry.current_function = name;
    guest_thread_state.entry.current_address = address;
    guest_checkpoint();
}
}

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

static void checkpoint_interval_contract() {
    require(sfr::guest_checkpoint_interval(nullptr) == 32, "unmeasured platforms retain the existing interval");
    require(sfr::guest_checkpoint_interval("256") == 256, "comparison interval is selectable");
    require(sfr::guest_checkpoint_interval("1") == 1 && sfr::guest_checkpoint_interval("4096") == 4096,
            "supported interval bounds are inclusive");
    for (const char* invalid : {"", "0", "-1", "+256", "256junk", " 256", "4097", "1000000", "4294967296"})
        require(sfr::guest_checkpoint_interval(invalid) == 32, "invalid intervals safely retain the baseline");
    for (const char* setting : {"1", "32", "64", "128", "256", "4096"}) {
        const auto interval = sfr::guest_checkpoint_interval(setting);
        sfr::test_checkpoint_interval = interval;
        sfr::test_checkpoint_calls = 0;
        sfr::guest_thread_state.entry = {};
        sfr::guest_thread_state.entry.observed = false;
        PPCContext context;
        for (uint32_t i=0; i<3*interval; ++i) {
            if (i%2) sfr::guest_checkpoint();
            else sfr::enter_function(context, "cadence", 0x82000000);
        }
        require(sfr::test_checkpoint_calls == 3 && sfr::guest_thread_state.entry.checkpoint_countdown == 0,
                "function entries and loop checkpoints share the exact interval");
        sfr::GuestExecution gate;
        auto lease = gate.enter(1);
        sfr::test_checkpoint_lease = lease.get();
        struct ResetLease { ~ResetLease() { sfr::test_checkpoint_lease = nullptr; } } reset;
        sfr::guest_checkpoint();
        gate.stop();
        uint32_t calls = 0;
        bool cancelled = false;
        for (; calls<interval; ++calls) {
            try {
                if (calls%2) sfr::guest_checkpoint();
                else sfr::enter_function(context, "cancel", 0x82000004);
            } catch (const sfr::GuestExecutionCancelled&) { cancelled = true; ++calls; break; }
        }
        require(cancelled && calls == interval, "stopping is observed by the next permit call, within one entry interval");
    }
    sfr::test_checkpoint_interval = 32;
    sfr::test_checkpoint_calls = 0;
    sfr::guest_thread_state.entry = {};
}

static void urgent_handoff_at_next_entry_interval() {
    for (const auto interval : {1u, 32u, 256u, 4096u}) {
        sfr::GuestExecution gate;
        gate.set_scheduling_quantum(std::chrono::hours(1));
        gate.set_urgent(3, true);
        auto owner = gate.enter(1);
        sfr::test_checkpoint_interval = interval;
        sfr::test_checkpoint_calls = 0;
        sfr::guest_thread_state.entry = {};
        sfr::guest_thread_state.entry.observed = false;
        sfr::test_checkpoint_lease = owner.get();
        struct ResetLease { ~ResetLease() { sfr::test_checkpoint_lease = nullptr; } } reset;
        sfr::guest_checkpoint();

        std::atomic<bool> ran = false;
        auto urgent = std::async(std::launch::async, [&] {
            auto lease = gate.enter(3);
            ran.store(true);
        });
        // Stop before the future's destructor waits if an assertion fails.
        struct StopOnExit { sfr::GuestExecution& gate; ~StopOnExit() { gate.stop(); } } stop{gate};
        gate.wait_until_ready(3);
        PPCContext context;
        for (uint32_t i = 1; i < interval; ++i) {
            if (i % 2) sfr::guest_checkpoint();
            else sfr::enter_function(context, "urgent-wait", 0x82000008);
        }
        require(!ran.load() && sfr::test_checkpoint_calls == 1,
                "inline entries retain the permit until the configured boundary");
        sfr::guest_checkpoint();
        require(ran.load() && sfr::test_checkpoint_calls == 2,
                "urgent guest runs at the next permit call despite an unexpired quantum");
        urgent.get();
    }
    sfr::test_checkpoint_interval = 32;
    sfr::test_checkpoint_calls = 0;
    sfr::guest_thread_state.entry = {};
}

int main() {
    try {
        checkpoint_interval_contract();
        urgent_handoff_at_next_entry_interval();
        sfr::GuestMemory memory;
        memory.map(0x10000, 0x1000);
        memory.store<uint32_t>(0x10000, 11);
        memory.store<uint32_t>(0x10080, 22);
        PPCContext context;
        sfr::enter_function(context, "parent", 0x82000000);
        require(sfr::guest_thread_state.entry.checkpoint_countdown == 31, "first entry checkpoints");
        sfr::enter_function(context, "parent-fast", 0x82000004);
        require(sfr::guest_thread_state.entry.checkpoint_countdown == 30 &&
                sfr::guest_thread_state.entry.current_address == 0x82000004, "fast entry preserves observation");
        sfr::guest_thread_state.entry.parallel = true;
        memory.load_reserved_word(0x10000);
        std::exception_ptr failure;
        std::thread worker([&] {
            try {
                require(sfr::guest_thread_state.entry.observed && sfr::guest_thread_state.entry.watched &&
                        !sfr::guest_thread_state.entry.parallel && sfr::guest_thread_state.entry.checkpoint_countdown == 0,
                        "new host thread starts with independent entry state");
                require(!memory.has_reservation(), "parent reservation is not inherited");
                memory.store<uint32_t>(0x10080, 23);
                PPCContext child;
                sfr::enter_function(child, "child", 0x82000008);
                memory.load_reserved_word(0x10080);
                sfr::enter_function(child, "child-reserved", 0x8200000c);
                require(memory.has_reservation(), "entry does not destroy reservation");
                bool rejected = false;
                try { memory.store<uint32_t>(0x10088, 99); }
                catch (const sfr::RuntimeStop& e) { rejected = e.category == "reservation-interference"; }
                require(rejected && memory.store_conditional_word(0x10080, 24),
                        "ordinary store stays forbidden until conditional store completes");
            } catch (...) { failure = std::current_exception(); }
        });
        worker.join();
        if (failure) std::rethrow_exception(failure);
        require(sfr::guest_thread_state.entry.parallel && sfr::guest_thread_state.entry.current_address == 0x82000004 &&
                sfr::guest_thread_state.entry.checkpoint_countdown == 30, "child leaves parent entry intact");
        require(memory.has_reservation() && memory.store_conditional_word(0x10000, 12),
                "child leaves parent reservation intact");
        require(memory.load<uint32_t>(0x10000) == 12 && memory.load<uint32_t>(0x10080) == 24,
                "both guest operations complete");
        std::cout << "Guest entry and reservation isolation passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
