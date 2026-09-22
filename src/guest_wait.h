#pragma once
#include "native_sync_objects.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>

namespace sfr {
class GuestMemory;
class GuestWait {
public:
    struct Prepared {
        uint32_t status;
        bool infinite;
        uint64_t milliseconds;
        std::unique_ptr<NativeSyncObjects::WaitHandle> target;
        // Independent of guest memory and registry; safe while execution is blocked.
        NativeSyncObjects::WaitResult wait(std::stop_token stop) const;
    };
    // other_handles resolves handles outside the sync-object range (thread
    // handles); it returns null for handles it does not own.
    using Resolver = std::function<std::unique_ptr<NativeSyncObjects::WaitHandle>(uint32_t)>;
    static Prepared prepare(GuestMemory& memory, NativeSyncObjects& native, uint32_t handle,
                            uint32_t mode, uint32_t alertable, uint32_t timeout,
                            const Resolver& other_handles = {});
};
}
