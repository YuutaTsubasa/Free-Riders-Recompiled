#pragma once

#include <functional>
#include <memory>

namespace plume {
struct RenderCommandQueue;
struct RenderDevice;
}

namespace sfr {
// Direct3D 12 by default; SFR_GRAPHICS=vulkan selects Vulkan (the only
// backend elsewhere than Windows). Chosen once per process: the shader
// cache prepares bytecode for this backend.
enum class GraphicsBackend { d3d12, vulkan };
GraphicsBackend selected_graphics_backend();
const char* graphics_backend_name(GraphicsBackend backend);

class NativeGraphics {
public:
    NativeGraphics();
    ~NativeGraphics();

    NativeGraphics(const NativeGraphics&) = delete;
    NativeGraphics& operator=(const NativeGraphics&) = delete;

    void initialize();
    [[nodiscard]] bool initialized() const noexcept;
    plume::RenderDevice& device();
    plume::RenderCommandQueue& queue();
    [[nodiscard]] GraphicsBackend backend() const noexcept;
    // Linux: the SDL window Plume's Vulkan instance was made for (hidden
    // until presentation shows it). Null on Windows.
    [[nodiscard]] void* host_window() const noexcept;
    void wait_idle(const std::function<bool()>& cancelled = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
