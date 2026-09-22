#pragma once
#include "guest_memory.h"
#include <cstdint>
#include <type_traits>

namespace sfr {
// PowerPC load/store "with update" forms (lbzux, lhau, lfsu, stbux, stfdu, ...).
// EA = rA + offset in full 64 bits; memory uses its low 32 bits. rA receives
// the EA only after the checked access succeeds, so a stopped access leaves
// every register unchanged.

// Exact lfs conversion: widen the single format without host floating point,
// so no rounding or denormal-flush mode can change the bits.
uint64_t single_to_double_bits(uint32_t single);

template<class T>
void load_update(GuestMemory& memory, uint64_t& destination, uint64_t& base, uint64_t offset) {
    static_assert(std::is_integral_v<T> && sizeof(T) <= 8);
    if (&destination == &base)
        throw RuntimeStop("invalid-load-update", base, "RT and RA must be distinct registers");
    const uint64_t effective = base + offset;
    const auto value = memory.load<std::make_unsigned_t<T>>(static_cast<uint32_t>(effective));
    // Signed T sign-extends (lha), unsigned T zero-extends (lbz/lhz/lwz/ld).
    if constexpr (std::is_signed_v<T>)
        destination = static_cast<uint64_t>(static_cast<int64_t>(static_cast<T>(value)));
    else
        destination = static_cast<uint64_t>(value);
    base = effective;
}

void load_single_update(GuestMemory& memory, uint64_t& destination_bits, uint64_t& base, uint64_t offset);
void load_double_update(GuestMemory& memory, uint64_t& destination_bits, uint64_t& base, uint64_t offset);

template<class T>
void store_update(GuestMemory& memory, uint64_t& base, uint64_t source, uint64_t offset) {
    static_assert(std::is_unsigned_v<T> && sizeof(T) <= 8);
    // source is taken by value: when RS is RA, the old RA value is stored.
    const uint64_t effective = base + offset;
    memory.store<T>(static_cast<uint32_t>(effective), static_cast<T>(source));
    base = effective;
}
}
