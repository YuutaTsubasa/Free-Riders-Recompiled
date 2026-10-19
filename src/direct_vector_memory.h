#pragma once
// lvlx/lvrx/stvlx/stvrx on guest memory read and written as base + address
// (SFR_DIRECT_MEMORY; diagnostic_hooks.h). They touch only the part of an
// aligned 16-byte block on one side of the address, so they never cross a
// page: the loads read the whole aligned block, which lies in the address's
// page, and the stores write only the selected bytes, one at a time and in
// order. Each matches its checked counterpart in vector_memory.cpp, which
// the generated code called for every one of them (1-3% of a race's main
// thread on Windows and the AYN Thor).
#include <cstdint>
#include <cstring>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace sfr::direct {
inline void load_vector_left(const uint8_t* base, uint32_t address, uint8_t (&destination)[16]) {
    const uint32_t offset = address & 15;
    const uint8_t* block = base + (address & ~15u);
#if defined(__ARM_NEON)
    static constexpr uint8_t reverse[16] = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
    // Element k takes byte 15 - k + offset; indices past 15 read as zero.
    const uint8x16_t indices = vaddq_u8(vld1q_u8(reverse), vdupq_n_u8(uint8_t(offset)));
    vst1q_u8(destination, vqtbl1q_u8(vld1q_u8(block), indices));
#else
    uint8_t bytes[16];
    std::memcpy(bytes, block, 16);
    for (uint32_t k = 0; k < 16; ++k) destination[k] = k >= offset ? bytes[offset + 15 - k] : 0;
#endif
}
inline void load_vector_right(const uint8_t* base, uint32_t address, uint8_t (&destination)[16]) {
    const uint32_t count = address & 15;
    const uint8_t* block = base + (address - count);
#if defined(__ARM_NEON)
    static constexpr uint8_t reverse[16] = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
    // Element i takes byte count - 1 - i; the rest wrap past 15 and read as zero.
    const uint8x16_t indices = vsubq_u8(vld1q_u8(reverse), vdupq_n_u8(uint8_t(16 - count)));
    vst1q_u8(destination, vqtbl1q_u8(vld1q_u8(block), indices));
#else
    uint8_t bytes[16];
    std::memcpy(bytes, block, 16);
    for (uint32_t i = 0; i < 16; ++i) destination[i] = i < count ? bytes[count - 1 - i] : 0;
#endif
}
inline void store_vector_left(uint8_t* base, uint32_t address, const uint8_t (&source)[16]) {
    const uint32_t count = 16 - (address & 15);
    volatile uint8_t* bytes = base + address;
    for (uint32_t i = 0; i < count; ++i) bytes[i] = source[15 - i];
}
inline void store_vector_right(uint8_t* base, uint32_t address, const uint8_t (&source)[16]) {
    const uint32_t count = address & 15;
    volatile uint8_t* bytes = base + (address - count);
    for (uint32_t i = 0; i < count; ++i) bytes[count - i - 1] = source[i];
}
}
