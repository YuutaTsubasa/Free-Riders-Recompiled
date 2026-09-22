#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace plume {
struct RenderCommandList;
struct RenderRect;
struct RenderTexture;
struct RenderViewport;
}

namespace sfr {
class NativeGraphics;
class NativeRasterState;

struct NativeClear {
    bool color = false;
    bool depth = false;
    bool stencil = false;
    std::array<float, 4> color_value{};
    float depth_value = 1.0f;
    uint8_t stencil_value = 0;
};

class NativePresentation {
public:
    NativePresentation(NativeGraphics& graphics, uint32_t width, uint32_t height);
    ~NativePresentation();

    NativePresentation(const NativePresentation&) = delete;
    NativePresentation& operator=(const NativePresentation&) = delete;

    [[nodiscard]] uint32_t width() const noexcept;
    [[nodiscard]] uint32_t height() const noexcept;
    plume::RenderTexture& color();
    plume::RenderTexture& depth();
    void clear(const NativeClear& clear, std::span<const plume::RenderRect> rectangles = {});
    void set_raster_state(const plume::RenderViewport&, const plume::RenderRect&);
    const NativeRasterState& raster_state() const noexcept;
    [[nodiscard]] std::vector<uint8_t> readback_color();
    // Presents the framebuffer. When the frame only drew into the top-left
    // area (a title rendering at a back buffer smaller than the framebuffer),
    // that area is stretched to the window, as a console's scaler does; zero
    // presents the whole framebuffer.
    void present(uint32_t area_width = 0, uint32_t area_height = 0);
    // The area the last present stretched, or the framebuffer size.
    [[nodiscard]] std::pair<uint32_t, uint32_t> presented_area() const noexcept;
    // Records commands against the color/depth framebuffer with the current
    // viewport and scissor into the frame's open command list.
    void record(const std::function<void(plume::RenderCommandList&)>& body);
    // Submits the recorded draws and clears and waits for them (and for a
    // frame still in flight), then runs the after_flush callbacks with
    // complete set (resource recycling).
    void flush();
    // Runs each wait for the GPU, process-wide (there is one presentation).
    // The default just waits; the diagnostic releases the guest execution
    // permit around it, so a guest worker can run while the frame renders
    // instead of queueing behind a thread that is only waiting
    // (docs/performance.md). While a wait is in flight, recording from any
    // thread is refused rather than corrupting the submitted list.
    static void set_gpu_wait(std::function<void(const std::function<void()>&)> wait);
    // Runs after each submission. complete: every submitted command has
    // finished. Otherwise a present submitted the frame without waiting for
    // it (the GPU runs a frame behind); the frame before it has finished.
    void after_flush(std::function<void(bool complete)> callback);
    void clear_after_flush();
    void pump_events();
    [[nodiscard]] bool close_requested() const noexcept;
    [[nodiscard]] void* window_handle() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
