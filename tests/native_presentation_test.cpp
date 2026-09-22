#include "native_graphics.h"
#include "native_presentation.h"

#include "plume_render_interface.h"

#ifdef _WIN32
#include "plume_d3d12.h"
#define NOMINMAX
#include <windows.h>
#endif

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Exception, class Operation>
bool rejects_with(Operation operation) {
    try { operation(); } catch (const Exception&) { return true; }
    return false;
}

// The native-handle checks are D3D12's; the behaviour checks run on both
// backends (SFR_GRAPHICS=vulkan).
bool d3d12() { return sfr::selected_graphics_backend() == sfr::GraphicsBackend::d3d12; }

void invalid_dimensions_are_rejected_before_a_window_exists() {
    sfr::NativeGraphics graphics;
    graphics.initialize();
#ifdef _WIN32
    const auto before = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
#endif
    require(rejects_with<std::invalid_argument>([&] { sfr::NativePresentation p(graphics, 0, 1); }), "zero width is rejected");
    require(rejects_with<std::invalid_argument>([&] { sfr::NativePresentation p(graphics, 1, 0); }), "zero height is rejected");
    require(rejects_with<std::invalid_argument>([&] { sfr::NativePresentation p(graphics, 8193, 1); }), "excessive width is rejected");
    require(rejects_with<std::invalid_argument>([&] { sfr::NativePresentation p(graphics, 1, 8193); }), "excessive height is rejected");
#ifdef _WIN32
    require(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) == before, "invalid dimensions create no window objects");
#endif
}

void creates_hidden_fixed_size_window_and_native_resources() {
    sfr::NativeGraphics graphics;
    graphics.initialize();
    sfr::NativePresentation presentation(graphics, 37, 23);
#ifdef _WIN32
    auto hwnd = static_cast<HWND>(presentation.window_handle());
    require(IsWindow(hwnd), "presentation owns a real HWND");
    require(!IsWindowVisible(hwnd), "presentation window starts hidden");
    RECT client{};
    require(GetClientRect(hwnd, &client), "presentation client rectangle is available");
    require(client.right == 37 && client.bottom == 23, "client rectangle has the requested dimensions");
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    // Resizable: present scales the framebuffer into whatever the window is.
    require((style & WS_THICKFRAME) != 0 && (style & WS_MAXIMIZEBOX) != 0, "presentation window can be resized");
#else
    require(presentation.window_handle() != nullptr, "presentation has a window");
#endif
    require(presentation.width() == 37 && presentation.height() == 23, "resource dimensions are retained");
#ifdef _WIN32
    if (d3d12()) {
        require(static_cast<plume::D3D12Texture*>(&presentation.color())->d3d != nullptr, "color is a native D3D12 texture");
        require(static_cast<plume::D3D12Texture*>(&presentation.depth())->d3d != nullptr, "depth is a native D3D12 texture");
    }
#endif
}

void require_pixels(const std::vector<uint8_t>& bytes, uint32_t width, std::array<uint8_t, 4> bgra) {
    require(bytes.size() % 4 == 0, "readback is tightly packed BGRA");
    for (std::size_t i = 0; i < bytes.size(); i += 4)
        require(std::memcmp(bytes.data() + i, bgra.data(), 4) == 0, "every GPU-read pixel has the requested BGRA value");
    require(bytes.size() >= static_cast<std::size_t>(width) * 4, "readback contains complete rows");
}

void full_color_clears_reach_gpu_memory() {
    sfr::NativeGraphics graphics;
    graphics.initialize();
    sfr::NativePresentation presentation(graphics, 19, 11);
    sfr::NativeClear clear{};
    clear.color = true;
    clear.color_value = {1.0f, 0.0f, 0.0f, 1.0f};
    presentation.clear(clear);
    require_pixels(presentation.readback_color(), 19, {0, 0, 255, 255});
    clear.color_value = {0.0f, 1.0f, 1.0f, 1.0f};
    presentation.clear(clear);
    require_pixels(presentation.readback_color(), 19, {255, 255, 0, 255});
}

void rectangle_clear_preserves_outside_pixels() {
    sfr::NativeGraphics graphics;
    graphics.initialize();
    sfr::NativePresentation presentation(graphics, 8, 6);
    sfr::NativeClear clear{};
    clear.color = true;
    clear.color_value = {0, 0, 0, 1};
    presentation.clear(clear);
    clear.color_value = {1, 1, 1, 1};
    const plume::RenderRect rect(2, 1, 6, 5);
    presentation.clear(clear, std::span<const plume::RenderRect>(&rect, 1));
    const auto pixels = presentation.readback_color();
    for (uint32_t y = 0; y < 6; ++y) for (uint32_t x = 0; x < 8; ++x) {
        const uint8_t expected = x >= 2 && x < 6 && y >= 1 && y < 5 ? 255 : 0;
        const auto offset = (static_cast<std::size_t>(y) * 8 + x) * 4;
        require(pixels[offset] == expected && pixels[offset + 1] == expected && pixels[offset + 2] == expected && pixels[offset + 3] == 255,
                "rectangle clear changes only pixels inside the rectangle");
    }
}

void invalid_clear_arguments_do_not_mutate_color() {
    sfr::NativeGraphics graphics;
    graphics.initialize();
    sfr::NativePresentation presentation(graphics, 4, 4);
    sfr::NativeClear clear{};
    clear.color = true;
    clear.color_value = {0.25f, 0.5f, 0.75f, 1};
    presentation.clear(clear);
    const auto before = presentation.readback_color();
    clear.color_value[0] = std::numeric_limits<float>::quiet_NaN();
    require(rejects_with<std::invalid_argument>([&] { presentation.clear(clear); }), "non-finite color is rejected");
    clear.color_value = {1, 0, 0, 1};
    const plume::RenderRect bad(3, 3, 2, 4);
    require(rejects_with<std::invalid_argument>([&] { presentation.clear(clear, std::span<const plume::RenderRect>(&bad, 1)); }), "unordered rectangle is rejected");
    require(presentation.readback_color() == before, "invalid clear leaves actual color texture unchanged");
    clear.color = false;
    clear.depth = true;
    clear.depth_value = 1.01f;
    require(rejects_with<std::invalid_argument>([&] { presentation.clear(clear); }), "out-of-range depth is rejected");
}

void depth_stencil_clear_executes_on_native_attachment() {
#ifdef _WIN32
    if (!d3d12()) return;  // reads the planes back with D3D12 copies
    sfr::NativeGraphics graphics;
    graphics.initialize();
    sfr::NativePresentation presentation(graphics, 5, 3);
    auto* native = static_cast<plume::D3D12Texture*>(&presentation.depth());
    require(native->desc.format == plume::RenderFormat::D32_FLOAT_S8_UINT, "depth texture has depth and stencil storage");
    sfr::NativeClear clear{};
    clear.depth = true;
    clear.stencil = true;
    clear.depth_value = 0.375f;
    clear.stencil_value = 0x5a;
    presentation.clear(clear);
    // Clears are recorded into the frame's command list; submit it before an
    // independent readback list reads the attachment.
    presentation.flush();
    require(native->layout == plume::RenderTextureLayout::DEPTH_WRITE, "GPU clear leaves the actual attachment in depth-write layout");

    const auto resource_desc = native->d3d->GetDesc();
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 2> footprints{};
    std::array<UINT, 2> row_counts{};
    std::array<UINT64, 2> row_bytes{};
    UINT64 total_bytes = 0;
    static_cast<plume::D3D12Device*>(&graphics.device())->d3d->GetCopyableFootprints(
        &resource_desc, 0, 2, 0, footprints.data(), row_counts.data(), row_bytes.data(), &total_bytes);
    require(footprints[0].Footprint.Format == DXGI_FORMAT_R32_TYPELESS && row_bytes[0] == 20 && row_counts[0] == 3,
            "D3D12 reports the expected depth-plane footprint");
    require(footprints[1].Footprint.Format == DXGI_FORMAT_R8_TYPELESS && row_bytes[1] == 5 && row_counts[1] == 3,
            "D3D12 reports the expected stencil-plane footprint");
    auto readback = graphics.device().createBuffer(plume::RenderBufferDesc::ReadbackBuffer(total_bytes));
    auto commands = graphics.queue().createCommandList();
    auto fence = graphics.device().createCommandFence();
    require(readback && static_cast<plume::D3D12Buffer*>(readback.get())->d3d, "depth-stencil readback owns a native D3D12 buffer");
    commands->begin();
    commands->barriers(plume::RenderBarrierStage::COPY,
        plume::RenderTextureBarrier(&presentation.depth(), plume::RenderTextureLayout::COPY_SOURCE));
    D3D12_TEXTURE_COPY_LOCATION depth_destination{};
    depth_destination.pResource = static_cast<plume::D3D12Buffer*>(readback.get())->d3d;
    depth_destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    depth_destination.PlacedFootprint = footprints[0];
    D3D12_TEXTURE_COPY_LOCATION stencil_destination{};
    stencil_destination.pResource = static_cast<plume::D3D12Buffer*>(readback.get())->d3d;
    stencil_destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    stencil_destination.PlacedFootprint = footprints[1];
    D3D12_TEXTURE_COPY_LOCATION depth_source{};
    depth_source.pResource = native->d3d;
    depth_source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    depth_source.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION stencil_source = depth_source;
    stencil_source.SubresourceIndex = 1;
    auto* native_commands = static_cast<plume::D3D12CommandList*>(commands.get())->d3d;
    native_commands->CopyTextureRegion(&depth_destination, 0, 0, 0, &depth_source, nullptr);
    native_commands->CopyTextureRegion(&stencil_destination, 0, 0, 0, &stencil_source, nullptr);
    commands->barriers(plume::RenderBarrierStage::GRAPHICS,
        plume::RenderTextureBarrier(&presentation.depth(), plume::RenderTextureLayout::DEPTH_WRITE));
    commands->end();
    graphics.queue().executeCommandLists(commands.get(), fence.get());
    graphics.queue().waitForCommandFence(fence.get());
    const auto* bytes = static_cast<const uint8_t*>(readback->map());
    require(bytes != nullptr, "depth-stencil GPU readback maps");
    for (uint32_t y = 0; y < 3; ++y) for (uint32_t x = 0; x < 5; ++x) {
        float depth_value = 0;
        std::memcpy(&depth_value, bytes + footprints[0].Offset + y * footprints[0].Footprint.RowPitch + x * 4, sizeof(depth_value));
        require(depth_value == 0.375f, "every GPU depth pixel contains the cleared value");
        require(bytes[footprints[1].Offset + y * footprints[1].Footprint.RowPitch + x] == 0x5a,
                "every GPU stencil pixel contains the cleared value");
    }
    readback->unmap();
#endif
}
}

int main() {
    try {
        invalid_dimensions_are_rejected_before_a_window_exists();
        creates_hidden_fixed_size_window_and_native_resources();
        full_color_clears_reach_gpu_memory();
        rectangle_clear_preserves_outside_pixels();
        invalid_clear_arguments_do_not_mutate_color();
        depth_stencil_clear_executes_on_native_attachment();
        std::cout << "Native presentation checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
