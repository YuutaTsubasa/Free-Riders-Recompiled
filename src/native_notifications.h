#pragma once

#include <cstdint>
#include <memory>

namespace sfr {
class GuestMemory;
class NativeSyncObjects;

class NativeNotifications {
public:
    NativeNotifications(GuestMemory& memory, NativeSyncObjects& sync_objects);
    ~NativeNotifications();
    NativeNotifications(const NativeNotifications&) = delete;
    NativeNotifications& operator=(const NativeNotifications&) = delete;

    // These registry and guest-memory operations require the current guest
    // execution permit. Destruction requires all native producers to be stopped.
    uint32_t create(uint64_t mask, uint32_t maximum_version);
    bool get_next(uint32_t handle, uint32_t match, uint32_t id_output,
                  uint32_t parameter_output);
    uint32_t close(uint32_t handle);
    bool owns(uint32_t handle) const;
    // Native producers may call publish concurrently. It touches only the
    // internal queues and retained native event capabilities.
    void publish(uint32_t id, uint32_t parameter);
    // The same, for a notification the system would only send once the title
    // has seen the one before it: the item joins a listener's queue when that
    // listener next drains, so a pair of opposite states (a system UI opening
    // and then closing) cannot arrive within one pump and cancel out.
    void publish_when_drained(uint32_t id, uint32_t parameter);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
