#pragma once
#include "native_thread.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace sfr {
class GuestMemory;
class GuestThreads {
public:
    static constexpr uint32_t object_type = 0x72300000;
    static bool is_handle_range(uint32_t handle) { return handle >= 0x72200000 && handle < object_type; }
    struct TlsTemplate { uint32_t slots, raw_address, data_size, raw_size; };
    struct Request {
        uint32_t handle_output, stack_size, id_output, startup, worker, argument, flags, parent_cpu;
        // A system thread with a host-driven entry (startup and worker are 0);
        // it still gets a real PCR, thread object, TLS and stack.
        bool host_driven = false;
    };
    struct State {
        uint32_t handle, id, pcr, thread_object, tls_static, tls_dynamic;
        uint32_t stack_limit, stack_base, startup, worker, argument;
    };
    struct Snapshot { State state; uint32_t native_id; bool suspended, entry_started; };
    using TargetValidator = std::function<bool(uint32_t)>;
    using EntryFactory = std::function<NativeThread::Entry(const State&)>;
    GuestThreads(GuestMemory& memory, TlsTemplate tls, uint32_t default_stack,
                 TargetValidator validator, EntryFactory factory);
    ~GuestThreads();
    GuestThreads(const GuestThreads&) = delete;
    GuestThreads& operator=(const GuestThreads&) = delete;
    uint32_t create(const Request& request);
    // started: the owned host thread left its creation suspension and runs now.
    struct ResumeResult { uint32_t status, previous, id; bool started = false; };
    ResumeResult resume(uint32_t handle, uint32_t previous_output);
    // NtSuspendThread. Guest code only runs while holding the execution
    // permit, so a suspension that the suspending thread lifts before it
    // releases the permit (suspend, retarget processor, resume) is observable
    // only through the suspend count. The host thread is not stopped, so a
    // suspension held across a blocking call does not pause the target.
    ResumeResult suspend(uint32_t handle, uint32_t previous_output);
    // For the title's completion-then-self-suspend worker only: register the
    // suspension before publishing completion. suspend_self consumes it once.
    ResumeResult prepare_self_suspend(uint32_t handle);
    ResumeResult suspend_self(uint32_t handle, uint32_t previous_output);
    // Obtain under the execution permit; invoke without it. The captured
    // record stays alive until shutdown has joined every worker.
    std::function<void(std::stop_token)> suspension_waiter(uint32_t handle) const;
    uint32_t guest_suspends(uint32_t handle) const;
    // Host handle of an open guest thread handle, or null.
    void* host_handle(uint32_t handle) const;
    // Open handle of the thread with this object, or 0 (e.g. the main thread).
    uint32_t handle_for_object(uint32_t object) const;
    bool owns_object(uint32_t object) const;
    void shutdown() noexcept;
    uint32_t reference(uint32_t handle, uint32_t type, uint32_t output);
    void dereference(uint32_t object);
    uint32_t references(uint32_t object) const;
    uint32_t close(uint32_t handle);
    int32_t set_priority(uint32_t object, int32_t increment);
    int32_t priority(uint32_t object) const;
    uint32_t set_affinity(uint32_t object, uint32_t mask, uint32_t previous_output);
    uint64_t host_affinity(uint32_t object) const;
    Snapshot snapshot(uint32_t handle) const;
    size_t size() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
