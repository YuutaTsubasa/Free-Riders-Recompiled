#pragma once
#include "guest_memory.h"
#include <array>
#include <utility>

namespace sfr {
namespace detail {
template<typename Context> inline constexpr std::array saved_gprs{&Context::r14, &Context::r15, &Context::r16, &Context::r17, &Context::r18, &Context::r19, &Context::r20, &Context::r21, &Context::r22, &Context::r23, &Context::r24, &Context::r25, &Context::r26, &Context::r27, &Context::r28, &Context::r29, &Context::r30, &Context::r31};
template<unsigned First, typename Context, size_t... I>
void save_gprs(uint8_t* bytes, const Context& ctx, std::index_sequence<I...>) {
    // One volatile access per original PPC instruction. Do not merge these.
    ((void)(*reinterpret_cast<volatile uint64_t*>(bytes + I * 8) =
        __builtin_bswap64((ctx.*saved_gprs<Context>[First - 14 + I]).u64)), ...);
    *reinterpret_cast<volatile uint32_t*>(bytes + sizeof...(I) * 8) = __builtin_bswap32(ctx.r12.u32);
}
template<unsigned First, typename Context, size_t... I>
void restore_gprs(const uint8_t* bytes, Context& ctx, std::index_sequence<I...>) {
    (((ctx.*saved_gprs<Context>[First - 14 + I]).u64 =
        __builtin_bswap64(*reinterpret_cast<const volatile uint64_t*>(bytes + I * 8))), ...);
    ctx.r12.u64 = __builtin_bswap32(*reinterpret_cast<const volatile uint32_t*>(bytes + sizeof...(I) * 8));
    ctx.lr = ctx.r12.u64;
}
template<unsigned First> bool eligible_stack(uint32_t sp) {
    static_assert(First >= 14 && First <= 31);
#if defined(_WIN32)
    // The checked path retains any requested write-combined store fences.
    if (GuestMemory::host_write_combining()) return false;
#endif
    return !(sp & 7) && sp >= 8 * (33 - First);
}
}
// On a boundary/special page the caller executes the original instructions,
// retaining partial stores/loads before a later fault. No checkpoint may run
// between the complete range check and these accesses.
template<unsigned First, typename Context>
bool try_save_gprs(GuestMemory& memory, const Context& ctx) {
    const uint32_t sp = ctx.r1.u32;
    if (!detail::eligible_stack<First>(sp)) return false;
    constexpr uint32_t distance = 8 * (33 - First);
    auto* bytes = memory.fast_write(sp - distance, distance - 4);
    if (!bytes) return false;
    detail::save_gprs<First>(bytes, ctx, std::make_index_sequence<32 - First>{});
    return true;
}
template<unsigned First, typename Context>
bool try_restore_gprs(GuestMemory& memory, Context& ctx) {
    const uint32_t sp = ctx.r1.u32;
    if (!detail::eligible_stack<First>(sp)) return false;
    constexpr uint32_t distance = 8 * (33 - First);
    const auto* bytes = memory.fast_read(sp - distance, distance - 4);
    if (!bytes) return false;
    detail::restore_gprs<First>(bytes, ctx, std::make_index_sequence<32 - First>{});
    return true;
}
}
