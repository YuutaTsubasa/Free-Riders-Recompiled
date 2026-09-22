#include "load_halfword_update.h"
#include "guest_memory.h"
namespace sfr {
void load_halfword_update(GuestMemory& memory, uint64_t& destination, uint64_t& base, int16_t displacement) {
    if (&destination == &base)
        throw RuntimeStop("invalid-lhzu", base, "RT and RA must be distinct registers");
    // Retain full64 EA on update; Xenon memory addressing uses its low32 bits.
    const uint64_t effective = base + static_cast<uint64_t>(static_cast<int64_t>(displacement));
    const uint16_t value = memory.load<uint16_t>(static_cast<uint32_t>(effective));
    // A failed checked read must leave both GPRs intact. Assignment to full64
    // zero-extends the halfword, including when its top bit is set.
    destination = value;
    base = effective;
}
}
