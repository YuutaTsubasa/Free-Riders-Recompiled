#include "ansi_string.h"

#include "guest_memory.h"

#include <bitset>
#include <mutex>

namespace sfr {
void initialize_ansi_string(GuestMemory& memory, uint32_t destination, uint32_t source) {
    memory.check_write(destination, 8);

    if (!source) {
        memory.store<uint16_t>(destination, 0);
        memory.store<uint16_t>(uint64_t(destination) + 2, 0);
        memory.store<uint32_t>(uint64_t(destination) + 4, 0);
        return;
    }

    uint16_t length = 0;
    bool terminated = false;
    for (uint64_t offset = 0; offset <= 65534; ++offset) {
        const uint64_t address = uint64_t(source) + offset;
        if (address >= GuestMemory::address_space_size)
            throw RuntimeStop("ansi-string-address", source,
                              "ANSI string crosses the guest address space");
        if (memory.load<uint8_t>(address) == 0) {
            length = static_cast<uint16_t>(offset);
            terminated = true;
            break;
        }
    }
    if (!terminated)
        throw RuntimeStop("ansi-string-length", source,
                          "ANSI strings longer than 65534 bytes are unsupported");

    memory.store<uint16_t>(destination, length);
    memory.store<uint16_t>(uint64_t(destination) + 2, static_cast<uint16_t>(length + 1));
    memory.store<uint32_t>(uint64_t(destination) + 4, source);
}

void initialize_unicode_string(GuestMemory& memory, uint32_t destination, uint32_t source) {
    memory.check_write(destination, 8);

    if (!source) {
        memory.store<uint16_t>(destination, 0);
        memory.store<uint16_t>(uint64_t(destination) + 2, 0);
        memory.store<uint32_t>(uint64_t(destination) + 4, 0);
        return;
    }

    // At most 32766 characters, so that MaximumLength still fits.
    uint16_t bytes = 0;
    bool terminated = false;
    for (uint64_t offset = 0; offset <= 65532; offset += 2) {
        const uint64_t address = uint64_t(source) + offset;
        if (address + 1 >= GuestMemory::address_space_size)
            throw RuntimeStop("unicode-string-address", source,
                              "UNICODE string crosses the guest address space");
        if (memory.load<uint16_t>(address) == 0) {
            bytes = static_cast<uint16_t>(offset);
            terminated = true;
            break;
        }
    }
    if (!terminated)
        throw RuntimeStop("unicode-string-length", source,
                          "UNICODE strings longer than 32766 characters are unsupported");

    memory.store<uint16_t>(destination, bytes);
    memory.store<uint16_t>(uint64_t(destination) + 2, static_cast<uint16_t>(bytes + 2));
    memory.store<uint32_t>(uint64_t(destination) + 4, source);
}

namespace {
// The allocated strings' pool: 128 slots of 512 bytes in a guest page range
// of the runtime's own (outside the title's heaps).
constexpr uint32_t pool_address = 0x71740000, slot_size = 512, slot_count = 128;
constexpr uint32_t status_success = 0, status_no_memory = 0xC0000017, status_buffer_overflow = 0x80000005;
std::mutex pool_lock;
std::bitset<slot_count> slots_used;
}

uint32_t unicode_string_to_ansi(GuestMemory& memory, uint32_t destination, uint32_t source, bool allocate) {
    const uint16_t bytes = memory.load<uint16_t>(source);
    const uint32_t characters = bytes / 2, text = memory.load<uint32_t>(uint64_t(source) + 4);
    uint32_t buffer;
    uint16_t maximum;
    if (allocate) {
        if (characters + 1 > slot_size) return status_no_memory;
        std::lock_guard lock(pool_lock);
        if (memory.available(pool_address, slot_size * slot_count)) memory.map(pool_address, slot_size * slot_count);
        uint32_t slot = 0;
        while (slot < slot_count && slots_used[slot]) ++slot;
        if (slot == slot_count) return status_no_memory;
        slots_used[slot] = true;
        buffer = pool_address + slot * slot_size;
        maximum = static_cast<uint16_t>(characters + 1);
    } else {
        buffer = memory.load<uint32_t>(uint64_t(destination) + 4);
        maximum = memory.load<uint16_t>(uint64_t(destination) + 2);
        if (characters + 1 > maximum) return status_buffer_overflow;
    }
    for (uint32_t i = 0; i < characters; ++i) {
        const uint16_t c = memory.load<uint16_t>(uint64_t(text) + 2 * i);
        memory.store<uint8_t>(uint64_t(buffer) + i, c < 0x80 ? uint8_t(c) : uint8_t('?'));
    }
    memory.store<uint8_t>(uint64_t(buffer) + characters, 0);
    memory.store<uint16_t>(destination, static_cast<uint16_t>(characters));
    memory.store<uint16_t>(uint64_t(destination) + 2, maximum);
    memory.store<uint32_t>(uint64_t(destination) + 4, buffer);
    return status_success;
}

void free_ansi_string(GuestMemory& memory, uint32_t string) {
    const uint32_t buffer = memory.load<uint32_t>(uint64_t(string) + 4);
    if (buffer >= pool_address && buffer < pool_address + slot_size * slot_count) {
        std::lock_guard lock(pool_lock);
        slots_used[(buffer - pool_address) / slot_size] = false;
    }
    memory.store<uint64_t>(string, 0);
}
}
