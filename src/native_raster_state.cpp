#include "native_raster_state.h"

#include "native_graphics.h"
#ifdef _WIN32
#include "plume_d3d12.h"
#endif
#include "plume_render_interface.h"

#include <cmath>
#include <stdexcept>

#ifndef _WIN32
// The same bounds as D3D12 (and Vulkan's usual viewportBoundsRange).
constexpr float D3D12_VIEWPORT_BOUNDS_MIN = -32768.0f, D3D12_VIEWPORT_BOUNDS_MAX = 32767.0f;
#endif

namespace {
bool valid_viewport_coordinate(float value) {
    return std::isfinite(value) && value >= D3D12_VIEWPORT_BOUNDS_MIN && value <= D3D12_VIEWPORT_BOUNDS_MAX;
}
}

namespace sfr {
NativeRasterState::NativeRasterState(NativeGraphics& graphics, uint32_t target_width, uint32_t target_height) {
    if (target_width == 0 || target_height == 0 || target_width > 8192 || target_height > 8192)
        throw std::invalid_argument("native raster target dimensions must be between 1 and 8192");

    // Vulkan allows minDepth above maxDepth; D3D12 reports whether it does.
    bool inverted_depth = true;
#ifdef _WIN32
    if (graphics.backend() == GraphicsBackend::d3d12) {
        auto* native_device = static_cast<plume::D3D12Device*>(&graphics.device());
        if (!native_device->d3d)
            throw std::runtime_error("native raster state requires an available D3D12 device");
        D3D12_FEATURE_DATA_D3D12_OPTIONS13 options{};
        if (FAILED(native_device->d3d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS13, &options, sizeof(options))))
            throw std::runtime_error("failed to query D3D12 inverted viewport depth support");
        inverted_depth = options.InvertedViewportDepthFlipsZSupported != FALSE;
    }
#endif

    target_width_ = target_width;
    target_height_ = target_height;
    viewport_ = plume::RenderViewport(0, 0, static_cast<float>(target_width), static_cast<float>(target_height), 0, 1);
    scissor_ = plume::RenderRect(0, 0, static_cast<int32_t>(target_width), static_cast<int32_t>(target_height));
    inverted_depth_supported_ = inverted_depth;
}

void NativeRasterState::set(const plume::RenderViewport& viewport, const plume::RenderRect& scissor) {
    if (!valid_viewport_coordinate(viewport.x) || !valid_viewport_coordinate(viewport.y) ||
        !std::isfinite(viewport.width) || !std::isfinite(viewport.height) ||
        viewport.width < 0 || viewport.height < 0 ||
        viewport.width > D3D12_VIEWPORT_BOUNDS_MAX || viewport.height > D3D12_VIEWPORT_BOUNDS_MAX ||
        !valid_viewport_coordinate(viewport.x + viewport.width) ||
        !valid_viewport_coordinate(viewport.y + viewport.height))
        throw std::invalid_argument("viewport is outside the coordinate bounds");
    if (!std::isfinite(viewport.minDepth) || !std::isfinite(viewport.maxDepth) ||
        viewport.minDepth < 0 || viewport.minDepth > 1 || viewport.maxDepth < 0 || viewport.maxDepth > 1)
        throw std::invalid_argument("viewport depth values must be finite and between zero and one");
    if (viewport.minDepth > viewport.maxDepth && !inverted_depth_supported_)
        throw std::runtime_error("inverted viewport depth is unsupported by the device");
    if (scissor.left < 0 || scissor.top < 0 || scissor.left > scissor.right || scissor.top > scissor.bottom ||
        static_cast<uint32_t>(scissor.right) > target_width_ || static_cast<uint32_t>(scissor.bottom) > target_height_)
        throw std::invalid_argument("scissor must be ordered and within the target bounds");

    viewport_ = viewport;
    scissor_ = scissor;
}

void NativeRasterState::apply(plume::RenderCommandList& command_list) const {
    command_list.setViewports(viewport_);
    command_list.setScissors(scissor_);
}

const plume::RenderViewport& NativeRasterState::viewport() const noexcept { return viewport_; }
const plume::RenderRect& NativeRasterState::scissor() const noexcept { return scissor_; }
bool NativeRasterState::inverted_depth_supported() const noexcept { return inverted_depth_supported_; }
}
