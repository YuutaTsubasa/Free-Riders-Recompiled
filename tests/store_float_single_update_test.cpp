#include "store_float_single_update.h"
#include "guest_memory.h"
#include <cfenv>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
template<class F> void rejects(F operation) {
    try { operation(); } catch(const sfr::RuntimeStop&) { return; }
    throw std::runtime_error("invalid stfsu did not stop");
}

void store_at(sfr::GuestMemory& memory,uint64_t bits,uint32_t expected,uint32_t address) {
    uint64_t base=address-4;
    sfr::store_float_single_update(memory,base,bits,4);
    require(base==address,"successful stfsu updates RA");
    require(memory.load<uint32_t>(address)==expected,"stfsu stores exact single bit pattern");
}

void conversion_vectors() {
    sfr::GuestMemory memory;
    memory.map(0x10000000,0x1000);
    struct Vector { uint64_t source; uint32_t expected; } vectors[] = {
        {0x0000000000000000ull,0x00000000u}, // +0
        {0x8000000000000000ull,0x80000000u}, // -0
        {0x3FF0000000000000ull,0x3F800000u}, // +1
        {0xBFF0000000000000ull,0xBF800000u}, // -1
        {0x3800000000000000ull,0x00400000u}, // E=896, upper subnormal boundary
        {0x380FFFFFFFFFFFFFull,0x007FFFFFu}, // E=896, largest subnormal result
        {0x36A0000000000000ull,0x00000001u}, // E=874, least nonzero result
        {0xB6AFFFFFFFFFFFFFull,0x80000001u}, // signed E=874, discarded tail
        {0x3810000000000000ull,0x00800000u}, // E=897, least normal
        {0x3FF0000018000000ull,0x3F800000u}, // truncates where host cast rounds up
        {0x4800000000000000ull,0x40000000u}, // overflow maps bits, never saturates
        {0x7FF0000000000000ull,0x7F800000u}, // +infinity
        {0xFFF0000000000000ull,0xFF800000u}, // -infinity
        {0x7FF0000000000001ull,0x7F800000u}, // tiny sNaN payload is not quieted
        {0x7FF0000020000000ull,0x7F800001u}, // surviving sNaN payload is not quieted
        {0x7FF8000000000000ull,0x7FC00000u}, // qNaN payload bits are copied
        {0x7FFFFFFFFFFFFFFFull,0x7FFFFFFFu},
        {0xFFFFFFFFFFFFFFFFull,0xFFFFFFFFu}
    };
    uint32_t address=0x10000020;
    for(const auto& vector:vectors) {
        store_at(memory,vector.source,vector.expected,address);
        address+=4;
    }
}

void rounding_mode_does_not_change_bits() {
    sfr::GuestMemory memory;
    memory.map(0x11000000,0x1000);
    const int original=std::fegetround();
    const int modes[]={FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO};
    uint32_t address=0x11000020;
    for(int mode:modes) {
        require(std::fesetround(mode)==0,"test host supports requested rounding mode");
        store_at(memory,0x3FF0000018000000ull,0x3F800000u,address);
        require(std::fegetround()==mode,"stfsu does not change host rounding mode");
        address+=4;
    }
    require(std::fesetround(original)==0,"restore host rounding mode");
}

void effective_address_and_guards() {
    sfr::GuestMemory memory;
    memory.map(0,4);
    memory.map(0x12000000,0x1004);
    memory.map(0xFFFFF000,0x1000);

    uint64_t base=0xFFFFFFFF12000004ull;
    sfr::store_float_single_update(memory,base,0x3FF0000000000000ull,-4);
    require(base==0xFFFFFFFF12000000ull && memory.load<uint32_t>(0x12000000)==0x3F800000u,
            "signed displacement uses full64 EA and low32 guest address");
    base=0xFFFFFFFCull;
    sfr::store_float_single_update(memory,base,0xBFF0000000000000ull,4);
    require(base==0x100000000ull && memory.load<uint32_t>(0)==0xBF800000u,
            "address carry remains in updated RA");
    base=UINT64_MAX-1;
    sfr::store_float_single_update(memory,base,0x3FF0000000000000ull,2);
    require(base==0 && memory.load<uint32_t>(0)==0x3F800000u,"full64 EA wraps modulo64");

    memory.store<uint8_t>(0x12001002,0xA5);
    base=0x12000FFE;
    rejects([&]{sfr::store_float_single_update(memory,base,0x3FF0000000000000ull,4);});
    require(base==0x12000FFE && memory.load<uint8_t>(0x12001002)==0xA5,
            "partial mapping rejects before bytes or RA change");

    memory.store<uint32_t>(0x12000040,0x11223344);
    unsigned provider_calls=0;
    memory.add_read_only_word(0x12000040,[&]{++provider_calls;return 0x11223344u;});
    base=0x1200003C;
    rejects([&]{sfr::store_float_single_update(memory,base,0x3FF0000000000000ull,4);});
    require(base==0x1200003C && provider_calls==0 &&
            memory.base()[0x12000040]==0x11 && memory.base()[0x12000043]==0x44,
            "read-only provider guard prevents sampling, write, and RA update");

    memory.load_reserved_word(0x12000080);
    base=0x1200008C;
    rejects([&]{sfr::store_float_single_update(memory,base,0x3FF0000000000000ull,4);});
    require(base==0x1200008C && memory.has_reservation(),
            "active reservation rejects ordinary store without consuming reservation");
}

void undefined_inputs_have_no_effect() {
    sfr::GuestMemory memory;
    memory.map(0x13000000,0x1000);
    const uint64_t undefined[]={
        0x0000000000000001ull, // positive double denormal
        0x800FFFFFFFFFFFFFull, // negative double denormal
        0x369FFFFFFFFFFFFFull, // largest nonzero E=873
        0xB690000000000000ull
    };
    for(uint64_t source:undefined) {
        memory.store<uint32_t>(0x13000020,0xA1B2C3D4u);
        uint64_t base=0x1300001C;
        rejects([&]{sfr::store_float_single_update(memory,base,source,4);});
        require(base==0x1300001C && memory.load<uint32_t>(0x13000020)==0xA1B2C3D4u,
                "undefined nonzero E<874 stops before store and RA update");
    }
}

#ifdef _WIN32
void write_combined_store() {
    sfr::GuestMemory memory;
    memory.map_write_combined(0x14000000,0x1000);
    store_at(memory,0x4000000000000000ull,0x40000000u,0x14000020);
}
#endif

void run() {
    conversion_vectors();
    rounding_mode_does_not_change_bits();
    effective_address_and_guards();
    undefined_inputs_have_no_effect();
#ifdef _WIN32
    write_combined_store();
#endif
}
}
int main() { try {run();return 0;} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;} }
