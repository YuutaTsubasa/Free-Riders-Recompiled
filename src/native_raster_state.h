#pragma once

#include "plume_render_interface_types.h"

#include <cstdint>

namespace plume {
struct RenderCommandList;
}

namespace sfr {
class NativeGraphics;

class NativeRasterState {
public:
    NativeRasterState(NativeGraphics& graphics, uint32_t target_width, uint32_t target_height);

    void set(const plume::RenderViewport& viewport, const plume::RenderRect& scissor);
    void apply(plume::RenderCommandList& command_list) const;

    [[nodiscard]] const plume::RenderViewport& viewport() const noexcept;
    [[nodiscard]] const plume::RenderRect& scissor() const noexcept;
    [[nodiscard]] bool inverted_depth_supported() const noexcept;

private:
    uint32_t target_width_ = 0;
    uint32_t target_height_ = 0;
    plume::RenderViewport viewport_;
    plume::RenderRect scissor_;
    bool inverted_depth_supported_ = false;
};
}
