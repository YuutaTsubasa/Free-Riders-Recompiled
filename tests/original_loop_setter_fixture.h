#pragma once
#include "guest_memory.h"
#include <array>
#include <bit>
#include <cstdint>
#include <stdexcept>

namespace sfr::test {
// Original image.bin instructions at 824EB9C8..824EBA18 (VS) and
// 824EBA20..824EBA70 (PS), cross-checked against diagnostic/ppc_recomp.51.cpp.
// The 88-byte regions (including their trailing zero word) have SHA-256:
// VS dc608a44a1df44c1c54bcdf63ea801cafeb0671ae8876172d5af1437b0810db0
// PS 9faf9f74aadf92b63ef43d13eef79e15d0d125b898c0a9ec91b398dd7f33fb84
// Only the first addi differs. Execute the original opcodes, including stwu's
// preincrement, rather than restating the host implementation's buffer offset.
// This deliberately supports only instructions present in these two setters.
inline void original_loop_setter(GuestMemory& memory, bool pixel, uint32_t device,
                                 uint32_t first, uint32_t source, uint32_t count) {
    if (!count || first >= 16 || count > 16-first)
        throw std::invalid_argument("original loop setter fixture range");
    constexpr std::array<uint32_t,21> code{
        0x394409e7,0x39650004,0x554a103a,0x7d4a1a14,0x892b0007,0x34c6ffff,0x890b0003,
        0x5529403e,0x88ebffff,0x396b0010,0x7d294378,0x5529402e,0x7d293b78,0x952a0004,
        0x4082ffd8,0x39800001,0xe9630020,0x798cc7e6,0x7d6b6378,0xf9630020,0x4e800020};
    std::array<uint64_t,32> r{};
    r[3]=device; r[4]=first; r[5]=source; r[6]=count;
    bool equal = false;
    uint32_t pc = 0;
    const auto unsupported = [] { throw std::logic_error("unexpected original loop setter instruction"); };
    for (unsigned steps=0; steps<256 && pc<code.size(); ++steps) {
        const uint32_t op = pixel && pc==0 ? 0x394409f7u : code[pc];
        const uint32_t rt=(op>>21)&31, ra=(op>>16)&31, rb=(op>>11)&31;
        const int32_t immediate = int16_t(op&0xffff);
        uint32_t next = pc+1;
        switch (op>>26) {
        case 14: // addi (including li)
            r[rt]=(ra ? r[ra] : 0)+immediate;
            break;
        case 13: // addic. -- only CR0.eq is consumed by this function
            r[rt]=r[ra]+immediate; equal=uint32_t(r[rt])==0;
            break;
        case 21: { // rlwinm / rotlwi; all three masks start at bit zero
            if (((op>>6)&31)!=0) unsupported();
            const uint32_t mask=0xffffffffu << (31-((op>>1)&31));
            r[ra]=std::rotl(uint32_t(r[rt]), int(rb))&mask;
            break;
        }
        case 31:
            if (((op>>1)&1023)==266) r[rt]=r[ra]+r[rb]; // add
            else if (((op>>1)&1023)==444) r[ra]=r[rt]|r[rb]; // or
            else unsupported();
            break;
        case 34: // lbz
            r[rt]=memory.load<uint8_t>(uint32_t(r[ra]+immediate));
            break;
        case 37: { // stwu: store at EA, THEN update the base register
            const uint32_t address=uint32_t(r[ra]+immediate);
            memory.store<uint32_t>(address,uint32_t(r[rt])); r[ra]=address;
            break;
        }
        case 16: // bne to the next int4, after addic. decremented the count
            if (op!=0x4082ffd8) unsupported();
            if (!equal) next=uint32_t(int32_t(pc)+int16_t(op&0xfffc)/4);
            break;
        case 58: // ld
            if (op!=0xe9630020) unsupported();
            r[rt]=memory.load<uint64_t>(uint32_t(r[ra]+immediate));
            break;
        case 30: // rldicr r12,r12,56,63 (full-width rotate, no masked bits)
            if (op!=0x798cc7e6) unsupported();
            r[12]=std::rotl(r[12],56);
            break;
        case 62: // std
            if (op!=0xf9630020) unsupported();
            memory.store<uint64_t>(uint32_t(r[ra]+immediate),r[rt]);
            break;
        case 19:
            if (op!=0x4e800020) unsupported(); // blr
            return;
        default: unsupported();
        }
        pc=next;
    }
    throw std::logic_error("original loop setter fixture did not return");
}
}
