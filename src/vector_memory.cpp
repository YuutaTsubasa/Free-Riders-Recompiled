#include "vector_memory.h"
#include "guest_memory.h"

namespace sfr {

VectorBytes load_vector_left(GuestMemory& memory, uint32_t address) {
    const uint32_t count = 16 - (address & 15);
    memory.check(address, count);
    VectorBytes result{};
    for (uint32_t i = 0; i < count; ++i)
        result[15 - i] = memory.load<uint8_t>(uint64_t(address) + i);
    return result;
}

VectorBytes load_vector_right(GuestMemory& memory, uint32_t address) {
    const uint32_t count = address & 15;
    VectorBytes result{};
    if (!count) return result;
    const uint32_t first = address - count;
    memory.check(first, count);
    for (uint32_t i = 0; i < count; ++i)
        result[i] = memory.load<uint8_t>(uint64_t(address) - i - 1);
    return result;
}

void store_vector_left(GuestMemory& memory, uint32_t address, const VectorBytes& value) {
    const uint32_t count = 16 - (address & 15);
    memory.check_write(address, count);
    for (uint32_t i = 0; i < count; ++i)
        memory.store<uint8_t>(uint64_t(address) + i, value[15 - i]);
}

void store_vector_right(GuestMemory& memory, uint32_t address, const VectorBytes& value) {
    const uint32_t count = address & 15;
    if (!count) return;
    memory.check_write(address - count, count);
    for (uint32_t i = 0; i < count; ++i)
        memory.store<uint8_t>(address - i - 1, value[i]);
}

VectorBytes load_vector_memory(GuestMemory& memory, uint32_t effective_address) {
    const uint32_t address = effective_address & ~uint32_t(0xf);
    // An aligned vector never crosses a page: one check, then the sixteen
    // bytes reversed (the register holds byte 15 of memory in element 0).
    if (const uint8_t* bytes = memory.fast_read(address, 16)) {
        VectorBytes result;
        for (unsigned i = 0; i < 16; ++i) result[i] = bytes[15 - i];
        return result;
    }
    memory.check(address, 16);
    const uint64_t first = memory.load<uint64_t>(address);
    const uint64_t second = memory.load<uint64_t>(uint64_t(address) + 8);

    VectorBytes result{};
    for (unsigned i = 0; i < 8; ++i) {
        result[i] = static_cast<uint8_t>(second >> (i * 8));
        result[i + 8] = static_cast<uint8_t>(first >> (i * 8));
    }
    return result;
}

void store_vector_memory(GuestMemory& memory, uint32_t effective_address, const VectorBytes& value) {
    const uint32_t address = effective_address & ~uint32_t(0xf);
    if (uint8_t* bytes = memory.fast_write(address, 16)) {
        for (unsigned i = 0; i < 16; ++i) bytes[i] = value[15 - i];
        return;
    }
    memory.check_write(address, 16);

    uint64_t first = 0;
    uint64_t second = 0;
    for (unsigned i = 0; i < 8; ++i) {
        first = (first << 8) | value[15 - i];
        second = (second << 8) | value[7 - i];
    }
    memory.store<uint64_t>(address, first);
    memory.store<uint64_t>(uint64_t(address) + 8, second);
}

void store_vector_word(GuestMemory& memory, uint32_t effective_address, const VectorBytes& value) {
    const uint32_t address = effective_address & ~uint32_t(3);
    const uint32_t index = 12 - (address & 12);
    uint32_t word = 0;
    for (uint32_t i = 0; i < 4; ++i)
        word |= uint32_t(value[index + i]) << (i * 8);
    memory.store<uint32_t>(address, word);
}

}
