#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <thread>

namespace sfr {

class GuestExecutionCancelled : public std::runtime_error {
public:
    GuestExecutionCancelled();
};

class GuestExecution {
    struct State;
public:
    class Lease {
    public:
        ~Lease() noexcept;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        void checkpoint(bool allow_handoff = true);
        // Starts a fresh scheduling quantum for the owner. A thread that has
        // just resumed another keeps running first, as the new hardware thread
        // needs time to start while its creator continues on its own core.
        void renew_quantum();
        // Releases the permit while operation performs native blocking only. The callback
        // must not access guest memory or runtime state. Normal return and callback errors
        // reacquire FIFO ownership first; cancellation throws without reacquiring the permit.
        // before_release runs after admission allocations, while still owning the permit.
        // It must not re-enter the scheduler. A thrown exception preserves ownership.
        void run_blocking(std::function<void(std::stop_token)> operation,
                          std::function<void()> before_release = {});
        // Lets the owner run on beside the permit's next owner, as a guest on
        // a console core of its own: until attach(), checkpoints only observe
        // cancellation. Only code that touches nothing but guest memory and
        // its own thread's state may run detached; anything reaching host
        // state (imports, hooks) must attach first. attach() queues for the
        // permit like a guest woken from a wait.
        void detach();
        void attach();
    private:
        void attach_self();
    public:
        bool detached() const { return detached_; }
        // A second permit this thread holds under this one (a console
        // core's, while this is the global permit), taken always after this
        // one: attach(), run_blocking() and a checkpoint's handoff release it
        // before they wait for this permit and take it back after. So a
        // thread holding the companion never waits for this permit.
        void set_companion(Lease* companion) { companion_ = companion; }
        // Runs at the end of each run_blocking, holding both permits again.
        void set_after_blocking(std::function<void()> after) { after_blocking_ = std::move(after); }

    private:
        friend class GuestExecution;
        Lease(std::shared_ptr<State> state, uint64_t identity);
        // Records a fresh ownership: owner thread and quantum start.
        void acquired();
        std::shared_ptr<State> state_;
        uint64_t guest_id_; // Internal identity; completion tokens are above UINT32_MAX.
        bool owns_ = false;
        std::atomic<std::thread::id> owner_thread_{};
        std::chrono::steady_clock::time_point acquired_at_{};
        std::chrono::steady_clock::time_point held_since_{};
        void released();  // accounts the hold that ends
        uint32_t checkpoints_ = 0;
        bool urgent_owner_ = false;
        bool detached_ = false;
        Lease* companion_ = nullptr;
        std::function<void()> after_blocking_;
    };
    // Minimum run time before a checkpoint hands the permit to a ready guest.
    // Zero (the default) hands off at every checkpoint while a guest is ready.
    void set_scheduling_quantum(std::chrono::microseconds quantum);
    // Time-critical guests preempt at the owner's next checkpoint when they
    // become ready after a wait (see State::enqueue).
    void set_urgent(uint32_t guest_id, bool urgent);
    // Nanoseconds guest 1 (the title's main thread) has spent ready to run
    // but queued behind another guest for the one permit: what serialising
    // the guest threads costs the frame, as opposed to the main thread
    // waiting on work it asked for.
    static std::atomic<uint64_t> main_thread_ready_wait_ns;
    // Nanoseconds guest 1 has spent in run_blocking operations: waits on
    // events, other threads and sleeps.
    static std::atomic<uint64_t> main_thread_blocked_ns;
    // Nanoseconds each guest (IDs below 64) has held the permit.
    static std::atomic<uint64_t> held_ns[64];

    GuestExecution();
    ~GuestExecution();
    GuestExecution(const GuestExecution&) = delete;
    GuestExecution& operator=(const GuestExecution&) = delete;

    std::unique_ptr<Lease> enter(uint32_t guest_id);
    // Host publication uses the same permit without adopting a guest/PPC identity.
    std::unique_ptr<Lease> enter_completion();
    // Called by the current owner after resuming guest_id; returns once that target is queued.
    void wait_until_ready(uint32_t guest_id);
    // follower stops whenever this execution stops or fails (a console
    // core's permit following the global one).
    void add_follower(GuestExecution& follower);
    // A stop token that fires when this execution stops, for blocking done
    // outside run_blocking (by a guest that holds only a follower's permit).
    std::stop_token stop_token() const;

    // Blocking waits each guest thread has entered through any permit's
    // run_blocking, process-wide. A thread another guest resumed first waits,
    // without the permit, until that resumer blocks once more (or `limit`
    // passes): the resumer may still be constructing the object the new
    // thread works on (a job queued and its worker resumed from a base
    // constructor), and on a console it finishes long before the new thread
    // starts. Our permit could hand over in between.
    static uint64_t blocking_waits(uint32_t guest_id);
    static void wait_for_block(uint32_t guest_id, uint64_t after, std::chrono::milliseconds limit,
                               std::stop_token stop);
    void fail(std::exception_ptr failure) noexcept;
    void stop() noexcept;
    void stop_and_drain() noexcept;
    void rethrow_failure() const;
    bool stopped() const;

private:
    std::unique_ptr<Lease> enter_identity(uint64_t identity);
    std::shared_ptr<State> state_;
};

}
