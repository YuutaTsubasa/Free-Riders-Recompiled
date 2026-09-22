#include "timestamp_bundle.h"
#include <string>
#include <utility>

namespace sfr {
TimestampBundle::TimestampBundle(GuestMemory& memory, std::function<uint32_t()> uptime) {
    if (!uptime) throw RuntimeStop("timestamp-bundle", address, "empty uptime provider");
    // Only +16 is confirmed by the observed title helper. Unknown prefix words
    // stay guarded and the remaining suffix is outside the logical mapping.
    memory.map(address, 20);
    for (uint32_t offset = 0; offset < 16; offset += 4)
        memory.add_import_variable(address + offset, "KeTimeStampBundle unknown field +" + std::to_string(offset));
    memory.add_read_only_word(address + 16, std::move(uptime));
}
}
