#include "diagnostic_hooks.h"
#include <atomic>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>

struct PPCContext {};
namespace sfr {
void guest_checkpoint_permit() { guest_thread_state.entry.checkpoint_countdown = 31; }
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

int main() {
    try {
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
