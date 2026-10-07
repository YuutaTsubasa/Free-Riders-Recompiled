#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <vector>

namespace sfr {

// Which of count host processors (in host_processor_order) guest processor
// guest_cpu takes. With six or more, its own. With fewer, guest processor 0
// keeps the first and 1-5 share the others: the main thread is pinned to the
// first (guest processor 0), and guest processor 4 landed there as well
// (4 % 4): on a 4-core, 4-thread i5-3470 the game ran 44% faster with the
// main thread unpinned (Issue #52). One host processor serves them all.
inline uint32_t host_processor_slot(uint32_t guest_cpu, size_t count) {
    if (count >= 6) return uint32_t(guest_cpu % count);
    if (guest_cpu == 0 || count <= 1) return 0;
    return 1 + (guest_cpu - 1) % uint32_t(count - 1);
}

#ifdef _WIN32
// The allowed logical processors of one group in the order guest processors
// take them (guest processor n gets entry host_processor_slot(n, count)): the first
// hardware thread of every physical core, fastest cores first, then the
// second threads. SFR_HOST_PROCESSORS=sequential gives plain bit order.
std::vector<uint32_t> host_processor_order(uint64_t allowed, uint16_t group);
// One-time placement of the calling guest thread within its current allowed
// group/mask. Explicit CPU Sets keep their current mask. Returns the resulting
// host mask; does not change process affinity.
uint64_t pin_current_guest_processor(uint32_t guest_cpu);
#endif

class NativeThread {
public:
    using Entry = std::function<uint32_t(std::stop_token)>;

    explicit NativeThread(Entry entry);
    ~NativeThread() noexcept;

    NativeThread(const NativeThread&) = delete;
    NativeThread& operator=(const NativeThread&) = delete;
    NativeThread(NativeThread&&) = delete;
    NativeThread& operator=(NativeThread&&) = delete;

    [[nodiscard]] uint32_t native_id() const;
    [[nodiscard]] bool entry_started() const;
    // Entry returned and published its result; join before reusing its storage.
    [[nodiscard]] bool completed() const;
    [[nodiscard]] bool suspended() const;
    // Host thread handle (signaled when the thread exits); owned by this object.
    [[nodiscard]] void* native_handle() const;
    uint32_t resume();
    uint32_t join();
    void request_stop() noexcept;
    void cancel_and_join() noexcept;
    [[nodiscard]] int32_t priority() const;
    int32_t set_priority(int32_t host_relative);
    uint64_t set_guest_processor(uint32_t guest_cpu);
#ifdef _WIN32
    // Prefer the mapped processor while allowing wakeups on the guest host pool.
    uint64_t set_guest_processor(uint32_t guest_cpu, bool allow_migration);
#endif
    [[nodiscard]] uint64_t affinity_mask() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
