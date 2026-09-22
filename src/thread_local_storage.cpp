#include "thread_local_storage.h"
#include <algorithm>

namespace sfr {
void ThreadLocalStorage::check_bank(uint32_t bank) const {
    if (std::find(banks_.begin(), banks_.end(), bank) == banks_.end())
        throw RuntimeStop("tls-bank", bank, "thread TLS value bank is not registered");
}
void ThreadLocalStorage::register_thread(uint32_t bank) {
    const uint64_t size = uint64_t(slot_count_) * 4;
    // The main thread plus one bank per guest thread slot (96).
    if (banks_.size() == 97) throw RuntimeStop("tls-bank", bank, "thread TLS bank capacity exhausted");
    for (const auto existing : banks_)
        if (bank < uint64_t(existing) + size && existing < uint64_t(bank) + size)
            throw RuntimeStop("tls-bank", bank, "thread TLS value banks overlap");
    memory_.check_write(bank, size);
    for (uint32_t index = 0; index < slot_count_; ++index)
        memory_.store<uint32_t>(uint64_t(bank) + index * 4, 0);
    banks_.push_back(bank); // Capacity reserved at construction, before memory publication.
}
void ThreadLocalStorage::clear_slot(uint32_t index) {
    for (const auto bank : banks_) memory_.check_write(uint64_t(bank) + index * 4, 4);
    for (const auto bank : banks_) memory_.store<uint32_t>(uint64_t(bank) + index * 4, 0);
}
uint32_t ThreadLocalStorage::allocate_for(uint32_t bank) {
    check_bank(bank);
    for (uint32_t index = 0; index < slot_count_; ++index) {
        if (allocated_[index]) continue;
        clear_slot(index);
        allocated_[index] = true;
        return index;
    }
    throw RuntimeStop("tls-capacity", bank, "process TLS slot capacity exhausted");
}
uint32_t ThreadLocalStorage::get_for(uint32_t bank, uint32_t index) const {
    check_bank(bank);
    (void)slot_address(index);
    return memory_.load<uint32_t>(uint64_t(bank) + index * 4);
}
uint32_t ThreadLocalStorage::set_for(uint32_t bank, uint32_t index, uint32_t value) {
    check_bank(bank);
    (void)slot_address(index);
    memory_.store<uint32_t>(uint64_t(bank) + index * 4, value);
    return 1;
}
ThreadLocalStorage::ThreadLocalStorage(GuestMemory& memory, uint32_t slot_count,
                                     uint32_t raw_address, uint32_t data_size, uint32_t raw_size)
    : memory_(memory), slot_count_(slot_count), data_size_(data_size) {
    // These are diagnostic quotas, not a claim about the full console TLS API.
    if (!slot_count || slot_count > allocated_.size() || data_size > 65536 || raw_size > data_size)
        throw RuntimeStop("tls-config", raw_address, "invalid TLS slot count or static data sizes");
    // Validate the entire raw range before reserving or mutating the destination.
    // A zero-length template needs no source mapping.
    if (raw_size) memory_.check(raw_address, raw_size);
    banks_.reserve(97);
    banks_.push_back(dynamic_address());
    const uint32_t total_size = data_size + slot_count * sizeof(uint32_t);
    // Whole pages: GuestMemory reads a partly committed page through its
    // checked path, and the main thread reads its TLS block millions of
    // times a race (docs/performance.md). The rest of the page stays zero.
    memory_.map(static_address, (total_size + 0xFFFu) & ~0xFFFu);
    memory_.check(static_address, total_size);
    for (uint32_t offset = 0; offset < raw_size; ++offset)
        memory_.store<uint8_t>(static_address + offset, memory_.load<uint8_t>(uint64_t(raw_address) + offset));
    for (uint32_t offset = raw_size; offset < total_size; ++offset)
        memory_.store<uint8_t>(static_address + offset, 0);
}

uint32_t ThreadLocalStorage::allocate() {
    return allocate_for(dynamic_address());
}

uint32_t ThreadLocalStorage::free(uint32_t index) {
    if (index == 0xffffffff) return 0;
    (void)slot_address(index);
    clear_slot(index);
    allocated_[index] = false;
    return 1;
}

uint32_t ThreadLocalStorage::get(uint32_t index) const {
    return get_for(dynamic_address(), index);
}

uint32_t ThreadLocalStorage::set(uint32_t index, uint32_t value) {
    return set_for(dynamic_address(), index, value);
}

uint32_t ThreadLocalStorage::slot_address(uint32_t index) const {
    if (index >= slot_count_ || !allocated_[index])
        throw RuntimeStop("tls-index", index, "invalid or unallocated TLS slot index");
    return dynamic_address() + index * sizeof(uint32_t);
}
}
