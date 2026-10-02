#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
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
struct AvatarFrameTransform;
struct AvatarPose;

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
    // Guest coordinates stay at width()/height(); attachments and readback use
    // these physical dimensions. Window output size is independently chosen.
    [[nodiscard]] uint32_t render_width() const noexcept;
    [[nodiscard]] uint32_t render_height() const noexcept;
    [[nodiscard]] plume::RenderRect render_rectangle(const plume::RenderRect&) const;
    plume::RenderTexture& color();
    plume::RenderTexture& depth();
    void clear(const NativeClear& clear, std::span<const plume::RenderRect> rectangles = {});
    void set_raster_state(const plume::RenderViewport&, const plume::RenderRect&);
    const NativeRasterState& raster_state() const noexcept;
    // Tightly packed BGRA at render_width() x render_height().
    [[nodiscard]] std::vector<uint8_t> readback_color();
    // Presents the framebuffer. When the frame only drew into the top-left
    // area (a title rendering at a back buffer smaller than the framebuffer),
    // that area is stretched to the window, as a console's scaler does; zero
    // presents the whole framebuffer.
    void present(uint32_t area_width = 0, uint32_t area_height = 0);
    void prepare_player_model();
    // Draw at the character pass using the title's current viewport/scissor.
    // Later title draws (including HUD) retain their original ordering.
    void draw_player_model(const AvatarFrameTransform& frame);
    std::optional<std::array<float, 16>> avatar_hand_transform(const AvatarPose&, uint32_t bone) const;
    // The area the last present stretched, or the framebuffer size.
    [[nodiscard]] std::pair<uint32_t, uint32_t> presented_area() const noexcept;
    // Records commands against the color/depth framebuffer with the current
    // viewport and scissor into the frame's open command list.
    void record(const std::function<void(plume::RenderCommandList&)>& body);
    // Like record(), but the commands are made by a render thread while the
    // caller goes on. body is copied into a queue slot, so it must be
    // trivially copyable and capture values, never references to the caller's
    // locals; it is called as body(list, generation) with the list_generation
    // the caller saw. Everything else that touches the command list (record,
    // clear, present, flush, ...) first waits for the queue to empty, so the
    // commands still land in the order they were asked for. SFR_RENDER_THREAD=0
    // runs them on the calling thread, straight away; unset, that is also what
    // every backend but D3D12 does (SFR_RENDER_THREAD=1 forces the thread).
    static constexpr size_t record_payload_bytes = 256;
    template <class Body>
    void record_async(const Body& body) {
        static_assert(std::is_trivially_copyable_v<Body> && sizeof(Body) <= record_payload_bytes,
                      "an asynchronous record body is copied into a fixed-size queue slot");
        record_async_raw([](const void* payload, plume::RenderCommandList& list, uint64_t generation) {
            (*static_cast<const Body*>(payload))(list, generation);
        }, &body, sizeof(Body));
    }
    // Changes when a list begins or a custom pass invalidates guest bindings.
    uint64_t list_generation() const;
    // Submits the recorded draws and clears and waits for them (and for a
    // frame still in flight), then runs the after_flush callbacks with
    // complete set (resource recycling).
    void flush();
    // Runs waits for unfinished GPU work, process-wide (one presentation).
    // Already-signaled fences are consumed without invoking this wrapper.
    // The default just waits; the diagnostic releases the guest execution
    // permit around it, so a guest worker can run while the frame renders
    // instead of queueing behind a thread that is only waiting
    // (docs/performance.md). While a wait is in flight, recording from any
    // thread is refused rather than corrupting the submitted list.
    static void set_gpu_wait(std::function<void(const std::function<void()>&)> wait);
    // Submit independent uploads on the same queue before recorded draws.
    // Also called by an empty flush, so pending copies cannot outlive teardown.
    void before_submit(std::function<void()> callback);
    void clear_before_submit();
    // Runs after each submission. complete: every submitted command has
    // finished. Otherwise a present submitted the frame without waiting for
    // it (the GPU runs a frame behind); the frame before it has finished.
    void after_flush(std::function<void(bool complete)> callback);
    void clear_after_flush();
    void pump_events();
    bool preparation_progress(size_t completed, size_t total, bool cancelling = false);
    void finish_preparation();
    [[nodiscard]] bool close_requested() const noexcept;
    [[nodiscard]] void* window_handle() const noexcept;

private:
    using RecordFunction = void (*)(const void* payload, plume::RenderCommandList&, uint64_t generation);
    void record_async_raw(RecordFunction function, const void* payload, size_t bytes);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
