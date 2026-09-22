#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string_view>
#include <vector>

namespace sfr {
class NativeSyncObjects {
public:
    struct CreateResult { uint32_t status, handle; };
    struct ReleaseResult { uint32_t status, previous_count; };
    struct WaitResult { uint32_t status; bool cancelled; };

    class WaitHandle {
    public:
        ~WaitHandle();
        WaitHandle(WaitHandle&&) noexcept;
        WaitHandle& operator=(WaitHandle&&) noexcept;
        WaitHandle(const WaitHandle&) = delete;
        WaitHandle& operator=(const WaitHandle&) = delete;

        WaitResult wait(uint32_t timeout_ms, std::stop_token stop = {});
        // KeWaitForMultipleObjects: WaitAny returns STATUS_WAIT_0 + index,
        // WaitAll returns STATUS_SUCCESS; both return STATUS_TIMEOUT on timeout.
        static WaitResult wait_multiple(const std::vector<WaitHandle*>& handles, bool wait_all,
                                        uint32_t timeout_ms, std::stop_token stop = {});

    private:
        struct Impl;
        explicit WaitHandle(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl_;
        friend class NativeSyncObjects;
    };

    class RetainedEvent {
    public:
        ~RetainedEvent();
        RetainedEvent(RetainedEvent&&) noexcept;
        RetainedEvent& operator=(RetainedEvent&&) noexcept;
        RetainedEvent(const RetainedEvent&) = delete;
        RetainedEvent& operator=(const RetainedEvent&) = delete;
        void reset();
        void signal();
    private:
        struct Impl;
        explicit RetainedEvent(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl_;
        friend class NativeSyncObjects;
    };

    NativeSyncObjects();
    ~NativeSyncObjects();
    NativeSyncObjects(const NativeSyncObjects&) = delete;
    NativeSyncObjects& operator=(const NativeSyncObjects&) = delete;

    CreateResult create_semaphore(int32_t initial, int32_t maximum, std::string_view name = {});
    CreateResult create_event(bool manual_reset, bool initial_state, std::string_view name = {});
    CreateResult create_notification_event();
    std::unique_ptr<RetainedEvent> retain_notification_event(uint32_t handle);
    std::unique_ptr<WaitHandle> retain_wait(uint32_t handle);
    // Waits on another host object (a guest thread's host thread, signaled
    // when it exits). The handle is duplicated; guest_handle names it in stops.
    static std::unique_ptr<WaitHandle> wait_on_host(void* host_handle, uint32_t guest_handle);
    // Acquire under GuestExecution ownership; the duplicate survives guest close.
    // Invalid handles return null; a non-event handle fails before any effects.
    std::unique_ptr<RetainedEvent> retain_event(uint32_t handle);
    uint32_t wait(uint32_t handle, uint32_t timeout_ms);
    ReleaseResult release_semaphore(uint32_t handle, int32_t adjustment);
    uint32_t set_event(uint32_t handle);
    uint32_t reset_event(uint32_t handle);
    uint32_t close(uint32_t handle);
    std::string_view name(uint32_t handle) const;
    bool owns(uint32_t handle) const;
    size_t open_count() const;
    static bool is_handle_range(uint32_t handle);

private:
    std::unique_ptr<RetainedEvent> retain_event_impl(uint32_t handle, bool notification);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
