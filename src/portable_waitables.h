#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>

namespace sfr::portable {
// The Windows kernel objects the guest's synchronization maps onto (events,
// semaphores, a thread's exit), for hosts without them. Every object shares
// one lock and one condition variable: a signal wakes all waiters, which
// re-check their objects. Waits follow WaitForMultipleObjects: wait-any
// takes the first signaled object in order, wait-all takes all of them at
// once; an auto-reset event and a semaphore are consumed by the wait that
// takes them.
enum class Kind { event, semaphore };

// State is guarded by the shared lock in portable_waitables.cpp.
class Waitable : public std::enable_shared_from_this<Waitable> {
public:
    explicit Waitable(Kind k) : kind(k) {}
    Kind kind;
    bool manual_reset = false;
    bool state = false;              // events
    int32_t count = 0, maximum = 0;  // semaphores
    bool signaled() const { return kind == Kind::event ? state : count > 0; }
    void take() {
        if (kind == Kind::semaphore) --count;
        else if (!manual_reset) state = false;
    }
};
using WaitablePtr = std::shared_ptr<Waitable>;

WaitablePtr make_event(bool manual_reset, bool initial_state);
WaitablePtr make_semaphore(int32_t initial, int32_t maximum);
// Signaled once, when the thread it stands for has finished (set_event).
WaitablePtr make_thread_exit();

void set_event(Waitable& event);
void reset_event(Waitable& event);
// False (and no change) when the count would pass the maximum; previous
// receives the count before the release.
bool release_semaphore(Waitable& semaphore, int32_t count, int32_t* previous);

constexpr uint32_t infinite = 0xFFFFFFFFu;
constexpr int wait_timeout = -1, wait_cancelled = -2;
// The index of the object taken (wait-any) or 0 (wait-all), wait_timeout,
// or wait_cancelled when stop is requested first.
int wait_any(std::span<Waitable* const> objects, uint32_t timeout_ms, std::stop_token stop = {});
int wait_all(std::span<Waitable* const> objects, uint32_t timeout_ms, std::stop_token stop = {});
}
