#pragma once
#include "native_graphics.h"
#include "plume_render_interface.h"
#include "screen_overlay.h"
#include <memory>

namespace sfr {
// Draws screen_overlay.h's layers over the frame in the presentation's blit
// pass. Each picture is uploaded once, on a command list of its own that is
// waited for (pictures change a few times a second at most: a notification
// appearing, the page scrolling), and the textures of the last few pictures
// are kept, so a frame still in flight never samples a texture being replaced.
class ScreenOverlayRenderer {
public:
    ScreenOverlayRenderer(plume::RenderDevice& device, plume::RenderCommandQueue& queue, GraphicsBackend backend,
                          plume::RenderFormat target_format);
    ~ScreenOverlayRenderer();
    ScreenOverlayRenderer(const ScreenOverlayRenderer&) = delete;
    ScreenOverlayRenderer& operator=(const ScreenOverlayRenderer&) = delete;
    // Uploads what is new (call before the pass that draws them). False if
    // the pipeline or a texture could not be made: nothing is drawn then.
    bool prepare(const std::vector<ScreenLayer>& layers);
    // Into the bound target of out_width x out_height, whose game picture is
    // the area (view_x, view_y, view_width, view_height) in its pixels.
    void draw(plume::RenderCommandList& list, const std::vector<ScreenLayer>& layers, float view_x, float view_y,
              float view_width, float view_height, float out_width, float out_height);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
