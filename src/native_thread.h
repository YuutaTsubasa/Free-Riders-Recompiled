#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>

namespace sfr {

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
    [[nodiscard]] uint64_t affinity_mask() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
