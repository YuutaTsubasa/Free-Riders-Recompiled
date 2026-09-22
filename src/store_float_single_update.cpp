#include "store_float_single_update.h"
#include "guest_memory.h"
namespace sfr {
void store_float_single_update(GuestMemory& memory, uint64_t& base,
                               uint64_t source_bits, int16_t displacement) {
    const uint64_t effective=base+static_cast<uint64_t>(static_cast<int64_t>(displacement));
    const uint32_t exponent=static_cast<uint32_t>((source_bits>>52)&0x7FF);
    uint32_t single_bits;
    if(exponent>896 || (source_bits&0x7FFFFFFFFFFFFFFFull)==0) {
        single_bits=static_cast<uint32_t>((source_bits>>32)&0xC0000000ull) |
                    static_cast<uint32_t>((source_bits>>29)&0x3FFFFFFFull);
    } else {
        if(exponent<874)
            throw RuntimeStop("floating-store",static_cast<uint32_t>(effective),
                              "undefined nonzero stfsu source exponent");
        const uint64_t significand=(uint64_t{1}<<52) |
                                   (source_bits&0x000FFFFFFFFFFFFFull);
        single_bits=static_cast<uint32_t>((source_bits>>32)&0x80000000ull) |
                    static_cast<uint32_t>(significand>>(926-exponent));
    }
    memory.store<uint32_t>(static_cast<uint32_t>(effective),single_bits);
    base=effective;
}
}
