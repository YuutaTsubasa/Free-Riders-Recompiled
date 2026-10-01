#include "vector_memory.h"
#include "guest_memory.h"
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace sfr {

VectorBytes load_vector_left(GuestMemory& memory, uint32_t address) {
    const uint32_t count = 16 - (address & 15);
#if defined(__aarch64__)
    // Only widen the host read when the entire aligned block is ordinary,
    // committed memory. Special words and short mappings retain the exact
    // selected-range path below, including its callback and failure order.
    if (const uint8_t* bytes = memory.fast_read(address & ~15u, 16)) {
        static constexpr uint8_t reverse[16] = {15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0};
        const uint8x16_t indices = vaddq_u8(vld1q_u8(reverse), vdupq_n_u8(address & 15));
        VectorBytes result;
        const uint8x16_t block = *reinterpret_cast<const volatile uint8x16_t*>(bytes);
        vst1q_u8(result.data(), vqtbl1q_u8(block, indices));
        return result;
    }
#endif
    if (const volatile uint8_t* bytes = memory.fast_read(address, count)) {
        VectorBytes result{};
        // Validate the selected tail once. Volatile byte reads retain the
        // checked path's access order without touching the excluded prefix.
        for (uint32_t i = 0; i < count; ++i) result[15 - i] = bytes[i];
        return result;
    }
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
#if defined(__aarch64__)
    if (const uint8_t* bytes = memory.fast_read(first, 16)) {
        static constexpr uint8_t reverse[16] = {15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0};
        const uint8x16_t indices = vsubq_u8(vld1q_u8(reverse), vdupq_n_u8(16 - count));
        const uint8x16_t block = *reinterpret_cast<const volatile uint8x16_t*>(bytes);
        vst1q_u8(result.data(), vqtbl1q_u8(block, indices));
        return result;
    }
#endif
    if (const volatile uint8_t* bytes = memory.fast_read(first, count)) {
        for (uint32_t i = 0; i < count; ++i) result[i] = bytes[count - i - 1];
        return result;
    }
    memory.check(first, count);
    for (uint32_t i = 0; i < count; ++i)
        result[i] = memory.load<uint8_t>(uint64_t(address) - i - 1);
    return result;
}

void store_vector_left(GuestMemory& memory, uint32_t address, const VectorBytes& value) {
    const uint32_t count = 16 - (address & 15);
#if defined(__aarch64__)
    // Check the exact range once, retaining write watches, reservations and
    // pending-I/O rejection. Keep each byte write and its original order.
    if (volatile uint8_t* bytes = memory.fast_write(address, count)) {
        for (uint32_t i = 0; i < count; ++i) bytes[i] = value[15 - i];
        return;
    }
#endif
    memory.check_write(address, count);
    for (uint32_t i = 0; i < count; ++i)
        memory.store<uint8_t>(uint64_t(address) + i, value[15 - i]);
}

void store_vector_right(GuestMemory& memory, uint32_t address, const VectorBytes& value) {
    const uint32_t count = address & 15;
    if (!count) return;
#if defined(__aarch64__)
    if (volatile uint8_t* bytes = memory.fast_write(address - count, count)) {
        for (uint32_t i = 0; i < count; ++i) bytes[count - i - 1] = value[i];
        return;
    }
#endif
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
