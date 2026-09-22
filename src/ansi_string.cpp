#include "ansi_string.h"

#include "guest_memory.h"

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
}
