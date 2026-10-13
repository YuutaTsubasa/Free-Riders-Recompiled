#pragma once
#include "guest_memory.h"
#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace sfr::test {
// Original image.bin instructions at the thirteen entries below, through blr
// (no padding), cross-checked against diagnostic/ppc_recomp.50.cpp. Concatenated
// in address order these 408 bytes have SHA-256:
// 682b095e842fa5a0f115966c825c3bac3cf6cdabaf62a39002f4e1201ab92488.
// Execute the original stores/rotate masks, not a second host stencil decoder.
inline void original_stencil_setter(GuestMemory& memory, uint32_t entry,
                                    uint32_t device, uint32_t value) {
    static constexpr uint32_t enable[]{0x81633158,0x81432934,0x216b0000,0x90832f18,
        0x7d6b5910,0x7d6b2038,0x516a07fe,0x91432934,0xe9630010,0x616b0800,
        0xf9630010,0x656b0002,0xf9630010,0x4e800020};
    static constexpr uint32_t back_enable[]{0x81632934,0x508b3e30,0x91632934,0xe9630010,
        0x616b0800,0xf9630010,0x656b0002,0xf9630010,0x4e800020};
    static constexpr uint32_t front_compare[]{0x81632934,0x508b456e,0x91632934,
        0xe9630010,0x616b0800,0xf9630010,0x4e800020};
    static constexpr uint32_t front_fail[]{0x81632934,0x508b5ca8,0x91632934,0xe9630010,
        0x616b0800,0xf9630010,0x656b0002,0xf9630010,0x4e800020};
    static constexpr uint32_t front_depth_fail[]{0x81632934,0x508b8b1c,0x91632934,0xe9630010,
        0x616b0800,0xf9630010,0x656b0002,0xf9630010,0x4e800020};
    static constexpr uint32_t front_pass[]{0x81632934,0x508b73e2,0x91632934,
        0xe9630010,0x616b0800,0xf9630010,0x4e800020};
    static constexpr uint32_t back_compare[]{0x81632934,0x508ba256,0x91632934,
        0xe9630010,0x616b0800,0xf9630010,0x4e800020};
    static constexpr uint32_t back_fail[]{0x81632934,0x508bb990,0x91632934,0xe9630010,
        0x616b0800,0xf9630010,0x656b0002,0xf9630010,0x4e800020};
    static constexpr uint32_t back_depth_fail[]{0x81632934,0x508be804,0x91632934,0xe9630010,
        0x616b0800,0xf9630010,0x656b0002,0xf9630010,0x4e800020};
    static constexpr uint32_t back_pass[]{0x81632934,0x508bd0ca,0x91632934,
        0xe9630010,0x616b0800,0xf9630010,0x4e800020};
    static constexpr uint32_t reference[]{0x98832903,0xe9630010,0x656b1000,0xf9630010,0x4e800020};
    static constexpr uint32_t read_mask[]{0x98832902,0xe9630010,0x656b1000,0xf9630010,0x4e800020};
    static constexpr uint32_t write_mask[]{0x98832901,0xe9630010,0x656b1000,0xf9630010,0x4e800020};
    std::span<const uint32_t> code;
    switch (entry) {
    case 0x824E7178: code=enable; break;
    case 0x824E71B8: code=back_enable; break;
    case 0x824E71F0: code=front_compare; break;
    case 0x824E7220: code=front_fail; break;
    case 0x824E7258: code=front_depth_fail; break;
    case 0x824E7290: code=front_pass; break;
    case 0x824E72C0: code=back_compare; break;
    case 0x824E72F0: code=back_fail; break;
    case 0x824E7328: code=back_depth_fail; break;
    case 0x824E7360: code=back_pass; break;
    case 0x824E7390: code=reference; break;
    case 0x824E73B0: code=read_mask; break;
    case 0x824E73D0: code=write_mask; break;
    default: throw std::invalid_argument("original stencil setter fixture entry");
    }
    std::array<uint64_t,32> r{};
    r[3]=device; r[4]=value;
    bool carry=false;
    const auto unsupported=[] { throw std::logic_error("unexpected original stencil setter instruction"); };
    for (uint32_t op : code) {
        const uint32_t rt=(op>>21)&31, ra=(op>>16)&31, rb=(op>>11)&31;
        const int32_t immediate=int16_t(op&0xffff);
        switch (op>>26) {
        case 32: r[rt]=memory.load<uint32_t>(uint32_t(r[ra]+immediate)); break; // lwz
        case 36: memory.store<uint32_t>(uint32_t(r[ra]+immediate),uint32_t(r[rt])); break;
        case 38: memory.store<uint8_t>(uint32_t(r[ra]+immediate),uint8_t(r[rt])); break;
        case 58: r[rt]=memory.load<uint64_t>(uint32_t(r[ra]+immediate)); break;
        case 62: memory.store<uint64_t>(uint32_t(r[ra]+immediate),r[rt]); break;
        case 24: r[ra]=r[rt]|(op&0xffff); break; // ori
        case 25: r[ra]=r[rt]|uint64_t((op&0xffff)<<16); break; // oris
        case 20: { // rlwimi; all masks here are contiguous
            const uint32_t begin=(op>>6)&31, end=(op>>1)&31;
            if (begin>end) unsupported();
            const uint32_t mask=(0xffffffffu>>begin)&(0xffffffffu<<(31-end));
            r[ra]=(r[ra]&~uint64_t(mask))|(std::rotl(uint32_t(r[rt]),int(rb))&mask);
            break;
        }
        case 8: // subfic r11,r11,0: carry iff attachment is zero
            if (op!=0x216b0000) unsupported();
            carry=r[11]==0; r[11]=0-r[11]; break;
        case 31:
            if (op==0x7d6b5910) r[11]=carry ? 0 : UINT64_MAX; // subfe r11,r11,r11
            else if (((op>>1)&1023)==28) r[ra]=r[rt]&r[rb]; // and
            else unsupported();
            break;
        case 19:
            if (op!=0x4e800020) unsupported();
            return;
        default: unsupported();
        }
    }
    throw std::logic_error("original stencil setter fixture did not return");
}
}
