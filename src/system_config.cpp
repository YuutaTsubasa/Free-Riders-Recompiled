#include "system_config.h"
#include "guest_memory.h"

namespace sfr {
SystemConfig::SystemConfig(GuestMemory& memory, uint32_t language, std::optional<uint32_t> country)
    : memory_(memory), language_(language), country_(country) {
    if (language < 1 || language > 12)
        throw RuntimeStop("system-config", language, "unsupported user language");
    if (country && (*country < 1 || *country > 109 || *country == 17 || *country == 94))
        throw RuntimeStop("system-config", *country, "unsupported user country");
}
uint32_t SystemConfig::query(uint16_t category, uint16_t setting, uint32_t buffer,
                             uint16_t capacity, uint32_t required) const {
    const bool language_query = category == 3 && setting == 9;
    const bool country_query = category == 3 && setting == 14;
    if (!language_query && !country_query)
        throw RuntimeStop("system-config", buffer, "unsupported configuration query");
    if (country_query && !country_)
        throw RuntimeStop("system-config", buffer, "user country is not configured");
    const uint16_t size = language_query ? 4 : 1;
    const uint32_t value = language_query ? language_ : *country_;
    uint32_t status = 0;
    if (buffer && capacity < size) status = 0xC0000023;
    else if (!buffer && capacity) status = 0xC00000F1;
    const bool write_value = status == 0 && buffer != 0;
    // Validate all effects before either store, including overlapping outputs.
    if (write_value) memory_.check_write(buffer, size);
    if (required) memory_.check_write(required, 2);
    if (write_value) {
        if (language_query) memory_.store<uint32_t>(buffer, value);
        else memory_.store<uint8_t>(buffer, static_cast<uint8_t>(value));
    }
    if (required) memory_.store<uint16_t>(required, status == 0 ? size : 0);
    return status;
}
}
