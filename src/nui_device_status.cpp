#include "nui_device_status.h"
#include "guest_memory.h"

namespace sfr {
void write_absent_nui_device_status(GuestMemory& memory, uint32_t output) {
    write_nui_device_status(memory, output, 0);
}

void write_nui_device_status(GuestMemory& memory, uint32_t output, uint32_t status) {
    // Preflight the complete ABI result before publishing any field. In the
    // absent profile all six BE32 words, including status at +12, are zero.
    memory.check_write(output, 24);
    for (uint32_t offset = 0; offset < 24; offset += 4)
        memory.store<uint32_t>(uint64_t(output) + offset, offset == 12 ? status : 0);
}
}
