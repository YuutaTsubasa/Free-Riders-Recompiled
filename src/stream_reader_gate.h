#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace sfr {
// The title's file stream (the manager at 0x83E58D80, built by 8249BE48) has
// two threads: a dispatcher (8249D158) that takes queued reads, and a reader
// (8249C100) that performs them. The dispatcher hands a read over in
// 8249D368: it stores the request at manager+144, sets manager+148, and
// signals the reader's thread event (thread object manager+132, +56). The
// reader's loop starts by clearing +148 and +144 and then waits on that event
// (824B2060, return address 8249C224); on waking it reads the request at +144.
//
// So a handoff made before the reader reaches that wait for the first time is
// lost: the reader clears it, and either wakes at once on the already signalled
// event and dereferences the null request (GUEST_ACCESS_VIOLATION guest=0x10
// in 8249C100), or - when the event was not armed yet - leaves the dispatcher
// thinking the read finished. 8249BE48 resumes the dispatcher first and the
// reader right after; on the console both start at once and the reader is
// waiting long before the first read arrives. Here new threads start later
// (start delay, waiting for their resumer to block) and a busy host (a Mac
// compiling Metal pipelines at boot) can hold the reader back past the
// dispatcher's first handoff. The gate makes the handoff wait for the reader.
namespace stream_reader {
inline constexpr uint32_t manager_reader_thread = 132;  // manager word: the reader's thread object
inline constexpr uint32_t manager_request = 144;        // manager word: the request handed over
inline constexpr uint32_t thread_resumed = 44;          // thread object byte: set by its resume (824B1DE0)
inline constexpr uint32_t thread_finished = 45;         // thread object byte: set once its function returned
inline constexpr uint32_t idle_wait_return = 0x8249C224; // the reader's call of the thread wait 824B2060
}

class StreamReaderGate {
public:
    // The reader whose thread object is given has cleared its request and is
    // about to wait for the next one.
    void reader_idle(uint32_t thread_object) {
        {
            std::lock_guard lock(mutex_);
            idle_.store(thread_object, std::memory_order_release);
        }
        changed_.notify_all();
    }
    // Whether the reader thread object `thread_object` has reached its wait.
    bool ready(uint32_t thread_object) const {
        return thread_object != 0 && idle_.load(std::memory_order_acquire) == thread_object;
    }
    // Waits until the reader that load_thread_object() names (reread each
    // time, as the dispatcher can run before the manager stores it) has reached
    // its wait, or `limit` passes. Returns whether it has. Call only inside the
    // runtime's host-wait wrapper, without the execution permit.
    template<class LoadThreadObject>
    bool wait_until_ready(LoadThreadObject&& load_thread_object, std::chrono::milliseconds limit) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        std::unique_lock lock(mutex_);
        for (;;) {
            if (ready(load_thread_object())) return true;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            // A bounded step: the thread object is guest memory, written
            // without notifying this gate.
            changed_.wait_for(lock, std::chrono::milliseconds(1));
        }
    }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::atomic<uint32_t> idle_{0};
};
}
