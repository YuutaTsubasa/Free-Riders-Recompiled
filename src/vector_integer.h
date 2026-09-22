#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>

namespace sfr::vmx {
// Element-wise VMX integer operations missing from the pinned XenonRecomp.
// V is XenonRecomp's PPCVRegister (or a test union with the same members).
// Registers hold the guest vector byte-reversed, which reverses the element
// order and makes each element host-endian. Element-wise operations are
// therefore unaffected: host lane i always pairs with host lane i. Results
// are built in a copy first because the destination may alias a source.
// Like XenonRecomp's saturating operations, these do not set VSCR[SAT].

template<class T>
T saturate(int64_t value) {
    return static_cast<T>(std::clamp<int64_t>(value, std::numeric_limits<T>::min(),
                                              std::numeric_limits<T>::max()));
}

#define SFR_VMX_LANEWISE(name, lanes, member, expression)                      \
    template<class V> void name(V& d, const V& a, const V& b) {                \
        V r = d;                                                                \
        for (int i = 0; i < lanes; ++i) {                                       \
            const auto x = a.member[i];                                         \
            const auto y = b.member[i];                                         \
            r.member[i] = expression;                                           \
        }                                                                       \
        d = r;                                                                  \
    }

// Shifts and rotates use only the low bits of each element of b.
SFR_VMX_LANEWISE(vslh, 8, u16, static_cast<uint16_t>(x << (y & 15)))
SFR_VMX_LANEWISE(vsrh, 8, u16, static_cast<uint16_t>(x >> (y & 15)))
SFR_VMX_LANEWISE(vsrah, 8, s16, static_cast<int16_t>(x >> (y & 15)))
SFR_VMX_LANEWISE(vrlh, 8, u16, static_cast<uint16_t>((x << (y & 15)) | (x >> ((16 - (y & 15)) & 15))))
SFR_VMX_LANEWISE(vsrab, 16, s8, static_cast<int8_t>(x >> (y & 7)))

SFR_VMX_LANEWISE(vaddsbs, 16, s8, saturate<int8_t>(int64_t{x} + y))
SFR_VMX_LANEWISE(vaddsws, 4, s32, saturate<int32_t>(int64_t{x} + y))
SFR_VMX_LANEWISE(vadduhs, 8, u16, saturate<uint16_t>(int64_t{x} + y))
SFR_VMX_LANEWISE(vsubshs, 8, s16, saturate<int16_t>(int64_t{x} - y))
SFR_VMX_LANEWISE(vsubuhs, 8, u16, saturate<uint16_t>(int64_t{x} - y))
SFR_VMX_LANEWISE(vsububm, 16, u8, static_cast<uint8_t>(x - y))

SFR_VMX_LANEWISE(vmaxsh, 8, s16, std::max(x, y))
SFR_VMX_LANEWISE(vminsh, 8, s16, std::min(x, y))
SFR_VMX_LANEWISE(vmaxuh, 8, u16, std::max(x, y))
SFR_VMX_LANEWISE(vminuh, 8, u16, std::min(x, y))
SFR_VMX_LANEWISE(vmaxuw, 4, u32, std::max(x, y))
SFR_VMX_LANEWISE(vminuw, 4, u32, std::min(x, y))
SFR_VMX_LANEWISE(vavguh, 8, u16, static_cast<uint16_t>((uint32_t{x} + y + 1) >> 1))

// Compares write all-ones or all-zeros elements into d.
SFR_VMX_LANEWISE(vcmpequh, 8, u16, static_cast<uint16_t>(x == y ? 0xFFFF : 0))
SFR_VMX_LANEWISE(vcmpgtsh, 8, s16, static_cast<int16_t>(x > y ? -1 : 0))
SFR_VMX_LANEWISE(vcmpgtuw, 4, u32, static_cast<uint32_t>(x > y ? 0xFFFFFFFFu : 0))
SFR_VMX_LANEWISE(vcmpgtsw, 4, s32, static_cast<int32_t>(x > y ? -1 : 0))

#undef SFR_VMX_LANEWISE

// Packs narrow the elements of vA and vB into one vector: guest elements
// 0..n-1 come from vA and n..2n-1 from vB. With reversed host lanes, host lane
// i < n is vB's lane i and host lane n + i is vA's lane i.
#define SFR_VMX_PACK(name, lanes, source, target, expression)                  \
    template<class V> void name(V& d, const V& a, const V& b) {                \
        V r = d;                                                                \
        for (int i = 0; i < lanes; ++i) {                                       \
            { const auto x = b.source[i]; r.target[i] = expression; }           \
            { const auto x = a.source[i]; r.target[lanes + i] = expression; }   \
        }                                                                       \
        d = r;                                                                  \
    }
SFR_VMX_PACK(vpkswss, 4, s32, s16, saturate<int16_t>(x))
SFR_VMX_PACK(vpkswus, 4, s32, u16, saturate<uint16_t>(x))
SFR_VMX_PACK(vpkuwum, 4, u32, u16, static_cast<uint16_t>(x))
SFR_VMX_PACK(vpkuwus, 4, u32, u16, saturate<uint16_t>(int64_t{x}))
SFR_VMX_PACK(vpkuhus, 8, u16, u8, saturate<uint8_t>(int64_t{x}))
SFR_VMX_PACK(vpkshss, 8, s16, s8, saturate<int8_t>(x))
#undef SFR_VMX_PACK

// vslo: shift vA left by whole octets; the count is bits 121:124 of vB (its
// last guest byte, host byte 0). Toward guest element 0 is toward host lane 15.
template<class V> void vslo(V& d, const V& a, const V& b) {
    const int shift = (b.u8[0] >> 3) & 15;
    V r = d;
    for (int i = 0; i < 16; ++i) r.u8[i] = i >= shift ? a.u8[i - shift] : uint8_t{0};
    d = r;
}

// Record-form compares set CR6: LT when every element is true, EQ when none is.
template<class V, class CR> void set_compare_cr6(CR& cr6, const V& result) {
    const bool all = result.u64[0] == UINT64_MAX && result.u64[1] == UINT64_MAX;
    const bool none = result.u64[0] == 0 && result.u64[1] == 0;
    cr6.lt = all;
    cr6.gt = 0;
    cr6.eq = none;
    cr6.so = 0;
}

// vcfpuxws128 (vctuxs): float * 2^scale truncated to an unsigned word,
// saturating; NaN and non-positive values give 0. The product is exact in
// double, and denormal inputs give 0 regardless of the host flush mode.
template<class V> void vcfpuxws(V& d, const V& b, unsigned scale) {
    V r = d;
    for (int i = 0; i < 4; ++i) {
        const double x = double(b.f32[i]) * double(uint64_t{1} << scale);
        r.u32[i] = !(x > 0) ? 0u : x >= 4294967296.0 ? 0xFFFFFFFFu : static_cast<uint32_t>(x);
    }
    d = r;
}

// vspltish: every halfword gets the sign-extended 5-bit immediate.
template<class V> void vspltish(V& d, int16_t value) {
    for (int i = 0; i < 8; ++i)
        d.s16[i] = value;
}

// vsel: take b's bits where c is set, a's bits elsewhere.
template<class V> void vsel(V& d, const V& a, const V& b, const V& c) {
    V r = d;
    for (int i = 0; i < 2; ++i)
        r.u64[i] = (a.u64[i] & ~c.u64[i]) | (b.u64[i] & c.u64[i]);
    d = r;
}
}
