#pragma once
#include <cstdint>
#include <cstring>

namespace sfr {
// One IEEE half float (1 sign, 5 exponent, 10 mantissa bits) as a float,
// subnormals, infinities and NaNs included.
inline float half_to_float(uint16_t half) {
    const uint32_t sign = uint32_t(half & 0x8000) << 16;
    uint32_t exponent = (half >> 10) & 0x1F;
    uint32_t mantissa = half & 0x3FF;
    uint32_t bits;
    if (exponent == 0x1F) {
        bits = sign | 0x7F800000 | (mantissa << 13);
    } else if (exponent) {
        bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    } else if (mantissa) {
        // Subnormal: normalize it.
        exponent = 113;
        while (!(mantissa & 0x400)) {
            mantissa <<= 1;
            --exponent;
        }
        bits = sign | (exponent << 23) | ((mantissa & 0x3FF) << 13);
    } else {
        bits = sign;
    }
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// vupkd3d128 of half floats (scripts/generate_diagnostic.py rewrites
// XenonRecomp's debug trap into this). XenonRecomp keeps a vector's elements
// in reverse order, so the guest's last halves are the first host ones:
// four halves (type 5) give X, Y, Z, W; two (type 3) give X, Y, 0, 1.
inline void vector_unpack_half(const uint16_t (&source)[8], float (&destination)[4], int halves) {
    float out[4];
    if (halves == 4) {
        for (int i = 0; i < 4; ++i) out[i] = half_to_float(source[i]);
    } else {
        out[3] = half_to_float(source[1]);  // X: guest half 6
        out[2] = half_to_float(source[0]);  // Y: guest half 7
        out[1] = 0.0f;
        out[0] = 1.0f;
    }
    std::memcpy(destination, out, sizeof(out));
}
}
