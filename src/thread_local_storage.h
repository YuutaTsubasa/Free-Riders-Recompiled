#pragma once
#include "guest_memory.h"
#include <array>

namespace sfr {
// One process-wide slot allocation pool with separate per-thread value banks.
// The guest execution gate serializes callers.
class ThreadLocalStorage {
public:
    static constexpr uint32_t static_address = 0x71300000;
    ThreadLocalStorage(GuestMemory& memory, uint32_t slot_count, uint32_t raw_address,
                       uint32_t data_size, uint32_t raw_size);
    ThreadLocalStorage(const ThreadLocalStorage&) = delete;
    ThreadLocalStorage& operator=(const ThreadLocalStorage&) = delete;
    uint32_t dynamic_address() const { return static_address + data_size_; }
    uint32_t allocate();
    uint32_t free(uint32_t index);
    uint32_t get(uint32_t index) const;
    uint32_t set(uint32_t index, uint32_t value);
    void register_thread(uint32_t bank);
    uint32_t allocate_for(uint32_t bank);
    uint32_t get_for(uint32_t bank, uint32_t index) const;
    uint32_t set_for(uint32_t bank, uint32_t index, uint32_t value);
private:
    uint32_t slot_address(uint32_t index) const;
    void check_bank(uint32_t bank) const;
    void clear_slot(uint32_t index);
    GuestMemory& memory_;
    uint32_t slot_count_;
    uint32_t data_size_;
    std::array<bool, 2048> allocated_{};
    std::vector<uint32_t> banks_;
};
}
