#include "native_blend_control.h"
#include <stdexcept>

namespace sfr {
namespace {

plume::RenderBlend decode_factor(uint32_t factor, bool alpha) {
    using B = plume::RenderBlend;
    switch (factor) {
    case 0: return B::ZERO;
    case 1: return B::ONE;
    case 4: return alpha ? B::SRC_ALPHA : B::SRC_COLOR;
    case 5: return alpha ? B::INV_SRC_ALPHA : B::INV_SRC_COLOR;
    case 6: return B::SRC_ALPHA;
    case 7: return B::INV_SRC_ALPHA;
    case 8: return alpha ? B::DEST_ALPHA : B::DEST_COLOR;
    case 9: return alpha ? B::INV_DEST_ALPHA : B::INV_DEST_COLOR;
    case 10: return B::DEST_ALPHA;
    case 11: return B::INV_DEST_ALPHA;
    case 12: return B::BLEND_FACTOR;
    case 13: return B::INV_BLEND_FACTOR;
    case 14:
    case 15:
        if (!alpha)
            throw std::invalid_argument("RGB constant-alpha blending requires separate scalar constant handling");
        return factor == 14 ? B::BLEND_FACTOR : B::INV_BLEND_FACTOR;
    case 16:
        if (alpha)
            throw std::invalid_argument("alpha source-alpha-saturate is outside the supported blend profile");
        return B::SRC_ALPHA_SAT;
    default:
        throw std::invalid_argument("reserved Xenos blend factor");
    }
}

plume::RenderBlendOperation decode_operation(uint32_t operation) {
    using O = plume::RenderBlendOperation;
    switch (operation) {
    case 0: return O::ADD;
    case 1: return O::SUBTRACT;
    case 4: return O::REV_SUBTRACT;
    case 2:
    case 3:
        // Xenos applies factors before MIN/MAX; D3D12 MIN/MAX ignores them.
        throw std::invalid_argument("Xenos factored MIN/MAX blending is not implemented");
    default:
        throw std::invalid_argument("reserved Xenos blend operation");
    }
}

}

NativeBlendControl decode_blend_control(uint32_t packed) {
    if (packed & 0xe000e000u)
        throw std::invalid_argument("nonzero padding in Xenos blend control");
    NativeBlendControl result;
    result.srcBlend = decode_factor(packed & 31, false);
    result.dstBlend = decode_factor((packed >> 8) & 31, false);
    result.blendOp = decode_operation((packed >> 5) & 7);
    result.srcBlendAlpha = decode_factor((packed >> 16) & 31, true);
    result.dstBlendAlpha = decode_factor((packed >> 24) & 31, true);
    result.blendOpAlpha = decode_operation((packed >> 21) & 7);
    // Only the exact copy tuple bypasses blending; even a copy-looking channel
    // must not disable a non-copy operation in the other channel.
    result.blendEnabled = !(result == NativeBlendControl{});
    return result;
}

plume::RenderBlendDesc NativeBlendControl::description(uint8_t write_mask) const {
    if (write_mask > 15)
        throw std::invalid_argument("blend write mask contains unsupported bits");
    plume::RenderBlendDesc result;
    result.blendEnabled = blendEnabled;
    result.srcBlend = srcBlend;
    result.dstBlend = dstBlend;
    result.blendOp = blendOp;
    result.srcBlendAlpha = srcBlendAlpha;
    result.dstBlendAlpha = dstBlendAlpha;
    result.blendOpAlpha = blendOpAlpha;
    result.renderTargetWriteMask = write_mask;
    return result;
}

}
