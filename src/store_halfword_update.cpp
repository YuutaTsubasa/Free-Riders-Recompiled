#include "store_halfword_update.h"
#include "guest_memory.h"
namespace sfr {
void store_halfword_update(GuestMemory& memory,uint64_t& base,uint64_t source,int16_t displacement) {
    // Xenon uses the low32 address for storage but retains the full64 computed
    // EA on update. Unsigned addition gives defined modular arithmetic.
    const uint64_t effective=base+static_cast<uint64_t>(static_cast<int64_t>(displacement));
    memory.store<uint16_t>(static_cast<uint32_t>(effective),static_cast<uint16_t>(source));
    // Failed preflight/store must not update RA. A by-value source preserves
    // the original low16 bits when RS and RA designate the same register.
    base=effective;
}
}
