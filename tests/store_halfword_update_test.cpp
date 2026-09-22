#include "store_halfword_update.h"
#include "guest_memory.h"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
template<class F> void rejects(F operation) {
    try { operation(); } catch(const sfr::RuntimeStop&) { return; }
    throw std::runtime_error("invalid store did not stop");
}
void run() {
    sfr::GuestMemory memory;
    memory.map(0,2);
    memory.map(0x10000000,0x10002);
    memory.map(0xFFFFF000,0x1000);
    uint64_t base=0xFFFFFFFF10000002;
    sfr::store_halfword_update(memory,base,0x112233445566ABCD,-2);
    require(base==0xFFFFFFFF10000000,"full64 base update, signed negative displacement");
    require(memory.load<uint8_t>(0x10000000)==0xAB && memory.load<uint8_t>(0x10000001)==0xCD,
            "low16 source is stored in big-endian order at low32 effective address");
    base=0x10000002;
    sfr::store_halfword_update(memory,base,base,2);
    require(base==0x10000004 && memory.load<uint16_t>(0x10000004)==2,
            "aliased source/base stores pre-update low16 bits");
    base=0xFFFFFFFE;
    sfr::store_halfword_update(memory,base,0x6789,2);
    require(base==0x100000000 && memory.load<uint16_t>(0)==0x6789,"carry into high32 is retained");
    base=UINT64_MAX-1;
    sfr::store_halfword_update(memory,base,0x1234,2);
    require(base==0 && memory.load<uint16_t>(0)==0x1234,"effective address arithmetic wraps at64 bits");
    base=0x10008000;
    sfr::store_halfword_update(memory,base,0x3210,-32768);
    require(base==0x10000000 && memory.load<uint16_t>(0x10000000)==0x3210,"signed16 minimum offset");
    base=0x10000001;
    sfr::store_halfword_update(memory,base,0x4321,32767);
    require(base==0x10008000 && memory.load<uint16_t>(0x10008000)==0x4321,"signed16 maximum offset");
    base=0x10000011;
    sfr::store_halfword_update(memory,base,0x9876,0);
    require(base==0x10000011 && memory.load<uint16_t>(0x10000011)==0x9876,
            "zero displacement and existing GuestMemory unaligned scalar behavior");
    base=0x20000000;
    rejects([&]{sfr::store_halfword_update(memory,base,0x2222,2);});
    require(base==0x20000000,"unmapped store leaves base unchanged");
    memory.store<uint8_t>(0x10010001,0xAA);
    base=0x1000FFFF;
    rejects([&]{sfr::store_halfword_update(memory,base,0x2233,2);});
    require(base==0x1000FFFF && memory.load<uint8_t>(0x10010001)==0xAA,
            "partially mapped halfword changes neither valid byte nor base");
    memory.store<uint8_t>(0xFFFFFFFF,0xBB);
    base=0xFFFFFFFD;
    rejects([&]{sfr::store_halfword_update(memory,base,0x3344,2);});
    require(base==0xFFFFFFFD && memory.load<uint8_t>(0xFFFFFFFF)==0xBB,
            "cross-address-space store is rejected without partial write");
    memory.store<uint32_t>(0x10000020,0x11223344);
    memory.add_read_only_word(0x10000020,[]{return 0x11223344u;});
    base=0x1000001F;
    rejects([&]{sfr::store_halfword_update(memory,base,0xFFFF,2);});
    require(base==0x1000001F && memory.load<uint32_t>(0x10000020)==0x11223344,
            "read-only overlap leaves base unchanged");
    require(memory.base()[0x10000020]==0x11 && memory.base()[0x10000021]==0x22 &&
            memory.base()[0x10000022]==0x33 && memory.base()[0x10000023]==0x44,
            "read-only provider cannot hide a partial backing-byte modification");
}
}
int main() { try {run();return 0;} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;} }
