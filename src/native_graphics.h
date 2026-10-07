#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace plume {
struct RenderCommandQueue;
struct RenderDevice;
}

namespace sfr {
// Direct3D 12 by default; SFR_GRAPHICS=vulkan selects Vulkan (the only
// backend elsewhere than Windows); a machine where no adapter can make
// Plume's D3D12 device falls back to Vulkan. Chosen once per process: the
// shader cache prepares bytecode for this backend.
enum class GraphicsBackend { d3d12, vulkan };
GraphicsBackend selected_graphics_backend();
const char* graphics_backend_name(GraphicsBackend backend);
// The driver version a backend reports, the way its vendor prints it: D3D12's
// user-mode driver version is four 16-bit parts (31.0.21912.14); Vulkan packs
// major.minor.patch in 10, 10 and 12 bits, except NVIDIA, whose major takes
// 10 bits and minor 8 (576.52.0). 0 (not reported) is "unknown".
std::string graphics_driver_version(GraphicsBackend backend, uint32_t vendor_id, uint64_t packed);

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
