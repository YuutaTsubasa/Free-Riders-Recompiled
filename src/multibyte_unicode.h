#pragma once

#include <cstdint>

namespace sfr {

class GuestMemory;

uint32_t multibyte_to_unicode_ascii(GuestMemory& memory,
                                    uint32_t destination,
                                    uint32_t destination_capacity,
                                    uint32_t written_output,
                                    uint32_t source,
                                    uint32_t input_bytes);

}
