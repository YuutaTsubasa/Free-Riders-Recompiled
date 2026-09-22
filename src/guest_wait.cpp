#include "guest_wait.h"
#include "guest_memory.h"
namespace sfr {
GuestWait::Prepared GuestWait::prepare(GuestMemory& memory, NativeSyncObjects& native,
        uint32_t handle, uint32_t mode, uint32_t alertable, uint32_t timeout, const Resolver& other_handles) {
    if (mode > 1 || alertable)
        throw RuntimeStop("sync-wait-profile", handle, "wait requires kernel/user mode and no alertable APC processing");
    std::unique_ptr<NativeSyncObjects::WaitHandle> other;
    if (handle && !NativeSyncObjects::is_handle_range(handle) && (!other_handles || !(other = other_handles(handle))))
        throw RuntimeStop("sync-wait-profile", handle, "wait supports owned event, semaphore and thread handles only");
    uint64_t milliseconds = 0;
    if (timeout) {
        const auto bits = memory.load<uint64_t>(timeout);
        if (bits && !(bits >> 63))
            throw RuntimeStop("sync-wait-profile", timeout, "absolute clock-tracking waits are not implemented");
        const auto magnitude = uint64_t(0) - bits;
        milliseconds = magnitude / 10000 + (magnitude % 10000 != 0);
    }
    auto target = other ? std::move(other) : native.retain_wait(handle);
    const uint32_t status = target ? 0 : 0xC0000008;
    return {status, timeout == 0, milliseconds, std::move(target)};
}
NativeSyncObjects::WaitResult GuestWait::Prepared::wait(std::stop_token stop) const {
    if (status) return {status, false};
    if (!target) throw std::logic_error("prepared wait has no retained target");
    if (infinite) return target->wait(0xFFFFFFFF, stop);
    auto remaining = milliseconds;
    for (;;) {
        // DWORD's largest value means INFINITE, never a finite timeout chunk.
        const uint32_t chunk = remaining > 0xFFFFFFFEull ? 0xFFFFFFFEu : uint32_t(remaining);
        const auto result = target->wait(chunk, stop);
        if (result.cancelled || result.status != 0x102 || remaining <= chunk) return result;
        remaining -= chunk;
    }
}
}
