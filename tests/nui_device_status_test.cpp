#include "guest_memory.h"
#include "nui_device_status.h"
#include <array>
#include <iostream>
#include <stdexcept>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class F> static void rejects(F action) {
    try { action(); } catch (const sfr::RuntimeStop&) { return; }
    throw std::runtime_error("invalid NUI output was accepted");
}
static void fill(sfr::GuestMemory& memory, uint32_t address, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) memory.store<uint8_t>(uint64_t(address) + i, 0xa5);
}
int main() {
    try {
        sfr::GuestMemory memory;
        memory.map(0x10000, 64);
        fill(memory, 0x10000, 64);
        sfr::write_absent_nui_device_status(memory, 0x10010);
        for (uint32_t i = 0; i < 64; ++i)
            require(memory.load<uint8_t>(0x10000 + i) == (i >= 16 && i < 40 ? 0 : 0xa5),
                    "only the complete six-word output may change");
        require(memory.load<uint32_t>(0x1001c) == 0, "original status field reports disconnected");
        memory.map(0xfffff000u, 0x1000);
        fill(memory, 0xffffffe8u, 24);
        sfr::write_absent_nui_device_status(memory, 0xffffffe8u);
        require(memory.load<uint8_t>(0xffffffffu) == 0, "last guest byte is writable without wrap");
        fill(memory, 0xffffffe8u, 24);
        rejects([&] { sfr::write_absent_nui_device_status(memory, 0xffffffe9u); });
        require(memory.load<uint8_t>(0xffffffe9u) == 0xa5, "overflow refuses before first write");
        rejects([&] { sfr::write_absent_nui_device_status(memory, 0); });

        for (unsigned guard = 0; guard < 3; ++guard) {
            sfr::GuestMemory protected_memory;
            protected_memory.map(0x20000, 32);
            fill(protected_memory, 0x20000, 32);
            sfr::GuestMemory::WriteLease pending;
            if (guard == 0) protected_memory.add_import_variable(0x20014, "NuiOutputTail");
            if (guard == 1) protected_memory.add_read_only_word(0x20014, [] { return 0x12345678u; });
            if (guard == 2) pending = protected_memory.pin_writes(std::array{sfr::GuestMemory::Range{0x20014, 4}});
            rejects([&] { sfr::write_absent_nui_device_status(protected_memory, 0x20000); });
            for (uint32_t i = 0; i < 20; ++i)
                require(protected_memory.load<uint8_t>(0x20000 + i) == 0xa5,
                        "late output conflict must preserve every earlier byte");
            require(protected_memory.load<uint8_t>(0x20018) == 0xa5, "neighbor remains unchanged");
        }
        std::cout << "Absent NUI device status checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
