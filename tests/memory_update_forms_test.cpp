#include "memory_update_forms.h"
#include "guest_memory.h"
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void stops(F operation, const char* message) {
    try { operation(); } catch (const sfr::RuntimeStop&) { return; }
    throw std::runtime_error(message);
}
uint64_t host_double_bits(float value) {
    const double widened = value;
    uint64_t bits;
    std::memcpy(&bits, &widened, sizeof bits);
    return bits;
}

void run() {
    using sfr::single_to_double_bits;
    require(single_to_double_bits(0x3F800000) == 0x3FF0000000000000, "1.0");
    require(single_to_double_bits(0xC0490FDB) == host_double_bits(-3.14159274f), "normal value");
    require(single_to_double_bits(0x80000000) == 0x8000000000000000, "negative zero");
    require(single_to_double_bits(0x7F800000) == 0x7FF0000000000000, "infinity");
    // A signaling NaN is widened, not quieted (bit 51 stays clear).
    require(single_to_double_bits(0x7F800001) == 0x7FF0000020000000, "signaling NaN payload");
    require(single_to_double_bits(0x7FC00000) == 0x7FF8000000000000, "quiet NaN");
    // Smallest denormal single is 2^-149; largest is (1 - 2^-23) * 2^-126.
    require(single_to_double_bits(0x00000001) == 0x36A0000000000000, "smallest denormal");
    require(single_to_double_bits(0x807FFFFF) == 0xB80FFFFFC0000000, "largest negative denormal");

    sfr::GuestMemory memory;
    memory.map(0x10000000, 0x1000);
    memory.store<uint32_t>(0x10000010, 0x8081FEFF);
    memory.store<uint32_t>(0x10000020, 0x3F800000);
    memory.store<uint64_t>(0x10000030, 0x400921FB54442D18);

    uint64_t destination = UINT64_MAX, base = 0x10000000;
    sfr::load_update<uint8_t>(memory, destination, base, 0x10);
    require(destination == 0x80 && base == 0x10000010, "lbzux zero-extends and updates");
    sfr::load_update<int16_t>(memory, destination, base, 0);
    require(destination == 0xFFFFFFFFFFFF8081 && base == 0x10000010, "lhau sign-extends");
    sfr::load_update<uint16_t>(memory, destination, base, 2);
    require(destination == 0xFEFF && base == 0x10000012, "lhzux");
    base = 0x10000010;
    sfr::load_update<uint32_t>(memory, destination, base, 0);
    require(destination == 0x8081FEFF, "lwzux zero-extends");
    base = 0x10000000;
    sfr::load_update<uint64_t>(memory, destination, base, 0x30);
    require(destination == 0x400921FB54442D18 && base == 0x10000030, "ldux");
    // Offsets wrap in 64 bits, as with a negative index register.
    base = 0x10000040;
    sfr::load_update<uint32_t>(memory, destination, base, static_cast<uint64_t>(-0x30));
    require(destination == 0x8081FEFF && base == 0x10000010, "negative offset");
    stops([&] { sfr::load_update<uint32_t>(memory, base, base, 0); }, "RT == RA must stop");

    uint64_t fpr = 0;
    base = 0x10000000;
    sfr::load_single_update(memory, fpr, base, 0x20);
    require(fpr == 0x3FF0000000000000 && base == 0x10000020, "lfsu");
    sfr::load_double_update(memory, fpr, base, 0x10);
    require(fpr == 0x400921FB54442D18 && base == 0x10000030, "lfdu");

    base = 0x10000100;
    sfr::store_update<uint8_t>(memory, base, 0x1234, 1);
    require(memory.load<uint8_t>(0x10000101) == 0x34 && base == 0x10000101, "stbux truncates");
    sfr::store_update<uint16_t>(memory, base, 0xABCD, 1);
    require(memory.load<uint16_t>(0x10000102) == 0xABCD && base == 0x10000102, "sthux big-endian");
    sfr::store_update<uint64_t>(memory, base, 0x0102030405060708, 6);
    require(memory.load<uint64_t>(0x10000108) == 0x0102030405060708 && base == 0x10000108, "stdux/stfdu");
    // RS == RA stores the old RA value.
    base = 0x10000110;
    sfr::store_update<uint32_t>(memory, base, base, 0x10);
    require(memory.load<uint32_t>(0x10000120) == 0x10000110 && base == 0x10000120, "RS == RA");

    // A stopped access changes neither register.
    destination = 7;
    base = 0x20000000;
    stops([&] { sfr::load_update<uint32_t>(memory, destination, base, 0); }, "unmapped load must stop");
    require(destination == 7 && base == 0x20000000, "failed load leaves registers");
    stops([&] { sfr::store_update<uint32_t>(memory, base, 1, 0); }, "unmapped store must stop");
    require(base == 0x20000000, "failed store leaves base");
    stops([&] { sfr::load_single_update(memory, fpr, base, 0); }, "unmapped lfsu must stop");
    require(base == 0x20000000 && fpr == 0x400921FB54442D18, "failed lfsu leaves registers");
}
}

int main() {
    try {
        run();
        std::cout << "memory update form tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
