#pragma once

#include "plume_render_interface_types.h"
#include <cstdint>

namespace sfr {

// RB_BLENDCONTROL contains no color write mask or blend constant values.
struct NativeBlendControl {
    plume::RenderBlend srcBlend = plume::RenderBlend::ONE;
    plume::RenderBlend dstBlend = plume::RenderBlend::ZERO;
    plume::RenderBlendOperation blendOp = plume::RenderBlendOperation::ADD;
    plume::RenderBlend srcBlendAlpha = plume::RenderBlend::ONE;
    plume::RenderBlend dstBlendAlpha = plume::RenderBlend::ZERO;
    plume::RenderBlendOperation blendOpAlpha = plume::RenderBlendOperation::ADD;
    bool blendEnabled = false;

    bool operator==(const NativeBlendControl&) const = default;
    plume::RenderBlendDesc description(uint8_t write_mask) const;
};

NativeBlendControl decode_blend_control(uint32_t packed);

}
