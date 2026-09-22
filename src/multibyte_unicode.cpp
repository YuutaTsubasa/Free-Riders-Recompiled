#include "multibyte_unicode.h"
#include "guest_memory.h"
#include <algorithm>
#include <vector>

namespace sfr {

namespace {
constexpr uint64_t maximum_units = 1024 * 1024;

bool overlaps(uint64_t first, uint64_t first_size, uint64_t second, uint64_t second_size) {
    return first_size && second_size && first < second + second_size && second < first + first_size;
}

void require_guest_range(uint32_t address, uint64_t size, const char* detail) {
    if (size > GuestMemory::address_space_size - uint64_t(address))
        throw RuntimeStop("multibyte-unicode", address, detail);
}
}

uint32_t multibyte_to_unicode_ascii(GuestMemory& memory,
                                    uint32_t destination,
                                    uint32_t destination_capacity,
                                    uint32_t written_output,
                                    uint32_t source,
                                    uint32_t input_bytes) {
    const uint64_t units = std::min<uint64_t>(input_bytes, destination_capacity / 2);
    if (units > maximum_units)
        throw RuntimeStop("multibyte-unicode", source, "ASCII conversion exceeds the bounded unit count");
    const uint64_t output_bytes = units * 2;

    require_guest_range(source, units, "source range wraps guest memory");
    require_guest_range(destination, output_bytes, "destination range wraps guest memory");
    if (written_output) require_guest_range(written_output, 4, "byte-count range wraps guest memory");

    if (overlaps(source, units, destination, output_bytes) ||
        (written_output && overlaps(source, units, written_output, 4)) ||
        (written_output && overlaps(destination, output_bytes, written_output, 4)))
        throw RuntimeStop("multibyte-unicode", destination, "conversion buffers overlap");

    if (units) memory.check(source, units);
    if (output_bytes) memory.check_write(destination, output_bytes);
    if (written_output) memory.check_write(written_output, 4);

    std::vector<uint8_t> input;
    input.reserve(static_cast<size_t>(units));
    for (uint64_t i = 0; i < units; ++i) {
        const uint8_t value = memory.load<uint8_t>(uint64_t(source) + i);
        if (value >= 0x80)
            throw RuntimeStop("multibyte-unicode", uint64_t(source) + i,
                              "Xbox multibyte code page is unknown outside ASCII");
        input.push_back(value);
    }

    for (uint64_t i = 0; i < units; ++i)
        memory.store<uint16_t>(uint64_t(destination) + i * 2, input[static_cast<size_t>(i)]);
    if (written_output) memory.store<uint32_t>(written_output, static_cast<uint32_t>(output_bytes));
    return 0;
}

}
