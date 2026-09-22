#pragma once

#include <cstdint>
#include <memory>
#include <stop_token>

namespace sfr {
class GuestMemory;

class GuestCriticalSections {
public:
    struct Shared;
    struct Waiter;
    class PendingEnter {
    public:
        // Abandoning an admitted, incomplete token makes the coordinator terminal
        // and wakes its waiters; it never silently removes a live guest waiter.
        ~PendingEnter();
        PendingEnter(PendingEnter&&) noexcept;
        PendingEnter& operator=(PendingEnter&&) noexcept;
        PendingEnter(const PendingEnter&) = delete;
        PendingEnter& operator=(const PendingEnter&) = delete;

        // Host-only wait. False means terminal cancellation or coordinator shutdown;
        // stopping wins over a racing ready notification.
        bool wait(std::stop_token stop = {});

    private:
        PendingEnter(std::shared_ptr<Shared> shared, std::shared_ptr<Waiter> waiter);
        std::shared_ptr<Shared> shared_;
        std::shared_ptr<Waiter> waiter_;
        friend class GuestCriticalSections;
    };

    explicit GuestCriticalSections(GuestMemory& memory);
    ~GuestCriticalSections();
    GuestCriticalSections(const GuestCriticalSections&) = delete;
    GuestCriticalSections& operator=(const GuestCriticalSections&) = delete;

    void initialize(uint32_t address);
    uint32_t initialize_and_spin_count(uint32_t address, uint32_t spin_count);
    // Null means the free or recursive acquisition completed immediately.
    std::unique_ptr<PendingEnter> enter(uint32_t address, uint32_t thread);
    void complete(PendingEnter& pending);
    void leave(uint32_t address, uint32_t thread);
    void stop() noexcept;

private:
    std::shared_ptr<Shared> shared_;
};
}
