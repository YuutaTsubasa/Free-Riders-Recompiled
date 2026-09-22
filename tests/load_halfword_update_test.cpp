#include "load_halfword_update.h"
#include "guest_memory.h"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F operation) {
    try { operation(); } catch (const sfr::RuntimeStop&) { return; }
    throw std::runtime_error("invalid load did not stop");
}
void run() {
    sfr::GuestMemory memory;
    memory.map(0, 2);
    memory.map(0x10000000, 0x10002);
    memory.map(0xFFFFF000, 0x1000);
    memory.store<uint8_t>(0x10000000, 0xAB);
    memory.store<uint8_t>(0x10000001, 0xCD);
    uint64_t destination = UINT64_MAX, base = 0xFFFFFFFF10000002;
    sfr::load_halfword_update(memory, destination, base, -2);
    require(destination == 0xABCD && base == 0xFFFFFFFF10000000,
            "big-endian read zero-extends full64 destination and updates full64 base");
    memory.store<uint16_t>(0, 0x6789);
    base = 0xFFFFFFFE;
    sfr::load_halfword_update(memory, destination, base, 2);
    require(destination == 0x6789 && base == 0x100000000,
            "low32 address with full64 carry");
    base = UINT64_MAX - 1;
    sfr::load_halfword_update(memory, destination, base, 2);
    require(destination == 0x6789 && base == 0, "modular full64 wrap");
    base = 0x10008000;
    sfr::load_halfword_update(memory, destination, base, -32768);
    require(destination == 0xABCD && base == 0x10000000, "signed16 minimum");
    memory.store<uint16_t>(0x10008000, 0x1234);
    base = 0x10000001;
    sfr::load_halfword_update(memory, destination, base, 32767);
    require(destination == 0x1234 && base == 0x10008000, "signed16 maximum");
    memory.store<uint16_t>(0x10000011, 0x9876);
    base = 0x10000011;
    sfr::load_halfword_update(memory, destination, base, 0);
    require(destination == 0x9876 && base == 0x10000011,
            "zero displacement and existing unaligned scalar policy");
    memory.add_read_only_word(0x10000020, [] { return 0x1122FEDCu; });
    base = 0x10000020;
    sfr::load_halfword_update(memory, destination, base, 2);
    require(destination == 0xFEDC && base == 0x10000022,
            "checked scalar read honors read-only value provider");
    destination = 0xFEDCBA9876543210;
    auto failed_read = [&](uint64_t initial, int16_t displacement) {
        base = initial;
        rejects([&] { sfr::load_halfword_update(memory, destination, base, displacement); });
        require(base == initial && destination == 0xFEDCBA9876543210,
                "failed read leaves both full64 registers unchanged");
    };
    failed_read(0x20000000, 2); // Unmapped.
    failed_read(0x1000FFFF, 2); // One mapped byte at end of logical region.
    failed_read(0xFFFFFFFD, 2); // Halfword crosses low32 address space.
    memory.reserve(0x30000000, 4096);
    failed_read(0x30000000, 0); // Reserved but uncommitted.
    memory.add_import_variable(0x10000030, "UnresolvedVariable");
    failed_read(0x1000002E, 2); // Protected import cannot be read as ordinary data.
    memory.add_read_only_word(0x10000040, []() -> uint32_t {
        throw sfr::RuntimeStop("test-provider", 0x10000040, "deliberate read failure");
    });
    failed_read(0x1000003E, 2);
    base = 0x10000000;
    rejects([&] { sfr::load_halfword_update(memory, base, base, 2); });
    require(base == 0x10000000, "invalid aliased RT/RA form is rejected without mutation");
}
}
int main() { try { run(); return 0; } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }
