#pragma once
#include <cstdint>
#include <memory>

namespace sfr {
class NativeSyncObjects;

// The kernel's waitable timers (NtCreateTimer, NtSetTimerEx, NtCancelTimer)
// over NativeSyncObjects: a timer is an event, auto-reset for a
// synchronization timer and manual-reset for a notification timer, that a
// host scheduler sets when it is due (and again every period). The title's
// network library (XNet, QoS and session upkeep) waits on them. Closing goes
// through the event's handle; a closed timer's later signals do nothing.
class GuestTimers {
public:
    explicit GuestTimers(NativeSyncObjects& objects);
    ~GuestTimers();
    GuestTimers(const GuestTimers&) = delete;
    GuestTimers& operator=(const GuestTimers&) = delete;

    // type 0: notification, 1: synchronization. Returns the status.
    uint32_t create(uint32_t type, uint32_t& handle);
    // due: 100 ns units, negative relative, positive absolute (FILETIME);
    // period in milliseconds (0: once). The timer is reset first.
    uint32_t set(uint32_t handle, int64_t due, uint32_t period_ms, bool& was_set);
    uint32_t cancel(uint32_t handle, bool& was_set);
    bool owns(uint32_t handle) const;
    void forget(uint32_t handle);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
