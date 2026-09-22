#include "hardware_info.h"

namespace sfr {
HardwareInfo::HardwareInfo(GuestMemory& memory) {
    // The pinned Xenia layout is larger, but the observed Free Riders boot path reads only the flags prefix.
    memory.map(address, sizeof(uint32_t));
    memory.store<uint32_t>(address, 0x20u);
}
}
