#include "hardware_info.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class F> bool rejects(F operation) {
    try { operation(); } catch (const sfr::RuntimeStop&) { return true; }
    return false;
}

void exposes_only_the_confirmed_free_riders_flags() {
    sfr::GuestMemory memory;
    sfr::HardwareInfo hardware_info(memory);

    constexpr std::array<uint8_t, 4> expected{0x00, 0x00, 0x00, 0x20};
    for (uint32_t i = 0; i < expected.size(); ++i)
        require(memory.load<uint8_t>(uint64_t(sfr::HardwareInfo::address) + i) == expected[i],
                "hardware flags use the expected big-endian bytes");
    require((memory.load<uint32_t>(sfr::HardwareInfo::address) & 0x10u) == 0,
            "Free Riders observed 0x10 mask is clear");

    require(rejects([&] { memory.load<uint8_t>(uint64_t(sfr::HardwareInfo::address) + 4); }),
            "bytes after flags are unmapped");
    require(rejects([&] { memory.store<uint8_t>(uint64_t(sfr::HardwareInfo::address) + 4, 0xff); }),
            "byte store after flags is rejected");
    require(memory.load<uint32_t>(sfr::HardwareInfo::address) == 0x20u,
            "rejected byte store after flags leaves flags unchanged");
    require(rejects([&] { memory.load<uint32_t>(uint64_t(sfr::HardwareInfo::address) + 1); }),
            "cross-tail word load is rejected");
    require(rejects([&] { memory.store<uint32_t>(uint64_t(sfr::HardwareInfo::address) + 1, 0xffffffffu); }),
            "cross-tail word store is rejected");
    require(memory.load<uint32_t>(sfr::HardwareInfo::address) == 0x20u,
            "rejected cross-tail store leaves flags unchanged");
    require(rejects([&] { memory.load<uint8_t>(uint64_t(sfr::HardwareInfo::address) - 1); }),
            "byte before flags is unmapped");
}

void rejects_existing_or_duplicate_mapping_without_overwrite() {
    {
        sfr::GuestMemory memory;
        memory.map(sfr::HardwareInfo::address, 4);
        memory.store<uint32_t>(sfr::HardwareInfo::address, 0xdecafbadu);

        require(rejects([&] { sfr::HardwareInfo hardware_info(memory); }),
                "preexisting mapping is rejected");
        require(memory.load<uint32_t>(sfr::HardwareInfo::address) == 0xdecafbadu,
                "preexisting mapping sentinel is preserved");
    }
    {
        sfr::GuestMemory memory;
        sfr::HardwareInfo first(memory);

        require(rejects([&] { sfr::HardwareInfo second(memory); }),
                "duplicate construction is rejected");
        require(memory.load<uint32_t>(sfr::HardwareInfo::address) == 0x20u,
                "duplicate construction leaves flags unchanged");
    }
}

void preserves_nearby_reservations() {
    sfr::GuestMemory memory;
    constexpr uint32_t page_size = 0x1000;
    constexpr uint32_t before = sfr::HardwareInfo::address - page_size;
    constexpr uint32_t after = sfr::HardwareInfo::address + page_size;
    memory.map(before, page_size);
    memory.map(after, page_size);
    memory.store<uint8_t>(uint64_t(sfr::HardwareInfo::address) - 1, 0x5a);
    memory.store<uint8_t>(after, 0xa5);

    sfr::HardwareInfo hardware_info(memory);

    require(memory.load<uint8_t>(uint64_t(sfr::HardwareInfo::address) - 1) == 0x5a,
            "preceding reservation sentinel is preserved");
    require(memory.load<uint8_t>(after) == 0xa5, "following reservation sentinel is preserved");
    require(memory.load<uint32_t>(sfr::HardwareInfo::address) == 0x20u, "flags remain readable");
}
}

int main() {
    try {
        exposes_only_the_confirmed_free_riders_flags();
        rejects_existing_or_duplicate_mapping_without_overwrite();
        preserves_nearby_reservations();
        std::cout << "Hardware info checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
