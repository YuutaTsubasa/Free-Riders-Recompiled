#include "native_presentation.h"
#include <functional>

#include "native_graphics.h"
#include "native_raster_state.h"
#include "touch_controls.h"
#ifdef _WIN32
#include "plume_d3d12.h"
#endif
#include "plume_vulkan.h"
#include "plume_render_interface.h"
#include "plume_render_interface_builders.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <SDL.h>
#ifdef __ANDROID__
#include <SDL_syswm.h>
#endif
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <vector>
#include <stdexcept>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <iostream>

// The blit shaders, compiled at build time (CMakeLists.txt, sfr_embed_shader).
#include "blit_vs_dxil.h"
#include "blit_ps_dxil.h"
#include "blit_vs_spirv.h"
#include "blit_ps_spirv.h"

namespace {
#ifdef _WIN32
constexpr wchar_t window_class_name[] = L"SonicFreeRidersNativePresentation";

// Borderless full screen over the window's monitor, and back to the window
// it was (Alt+Enter toggles, SFR_FULLSCREEN=1 starts in it).
constexpr DWORD windowed_style = WS_OVERLAPPEDWINDOW;
void set_fullscreen(HWND window, bool fullscreen) {
    static WINDOWPLACEMENT windowed{sizeof(WINDOWPLACEMENT)};
    const bool now = (GetWindowLongW(window, GWL_STYLE) & WS_POPUP) != 0;
    if (now == fullscreen) return;
    if (fullscreen) {
        GetWindowPlacement(window, &windowed);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &monitor);
        SetWindowLongW(window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        const RECT& r = monitor.rcMonitor;
        SetWindowPos(window, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongW(window, GWL_STYLE, windowed_style | WS_VISIBLE);
        SetWindowPlacement(window, &windowed);
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
}

LRESULT CALLBACK presentation_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_SYSKEYDOWN && wparam == VK_RETURN && (lparam & (LPARAM(1) << 29))) {
        set_fullscreen(window, !(GetWindowLongW(window, GWL_STYLE) & WS_POPUP));
        return 0;
    }
    if (message == WM_SYSCHAR && wparam == VK_RETURN) return 0;  // no beep for Alt+Enter
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    if (message == WM_CLOSE) {
        if (auto* requested = reinterpret_cast<bool*>(GetWindowLongPtrW(window, GWLP_USERDATA)))
            *requested = true;
        return 0;
    }
    if (message == WM_GETMINMAXINFO) {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        limits->ptMinTrackSize = POINT{1, 1};
        return 0;
    }
    if (message == WM_NCDESTROY)
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    return DefWindowProcW(window, message, wparam, lparam);
}

void ensure_window_class() {
    static std::once_flag once;
    static DWORD error = ERROR_SUCCESS;
    std::call_once(once, [] {
        // Window sizes are real pixels, not ones Windows scales up blurred.
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        WNDCLASSEXW descriptor{};
        descriptor.cbSize = sizeof(descriptor);
        descriptor.lpfnWndProc = presentation_window_proc;
        descriptor.hInstance = GetModuleHandleW(nullptr);
        descriptor.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        descriptor.lpszClassName = window_class_name;
        if (!RegisterClassExW(&descriptor)) error = GetLastError();
    });
    if (error != ERROR_SUCCESS && error != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("failed to register the native presentation window class");
}
#else
// Borderless full screen on the window's display (Alt+Enter toggles).
void set_fullscreen(SDL_Window* window, bool fullscreen) {
    SDL_SetWindowFullscreen(window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}
#endif

[[noreturn]] void unavailable(const char* object) {
    throw std::runtime_error(std::string("failed to create native presentation ") + object);
}
}

namespace sfr {
namespace {
// The blit draws into the swap chain: Android surfaces are RGBA, desktop ones BGRA.
#ifdef __ANDROID__
constexpr auto swap_chain_format = plume::RenderFormat::R8G8B8A8_UNORM;
#else
constexpr auto swap_chain_format = plume::RenderFormat::B8G8R8A8_UNORM;
#endif
}
struct NativePresentation::Impl {
    NativeGraphics* graphics = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
#ifdef _WIN32
    HWND window = nullptr;
#else
    SDL_Window* window = nullptr;  // NativeGraphics owns it
#endif
    bool close_requested = false;
    bool shown = false;
    bool start_fullscreen = false;
    bool ready = false;
    std::unique_ptr<NativeRasterState> raster_state;
    std::unique_ptr<plume::RenderSwapChain> swap_chain;
    std::unique_ptr<plume::RenderTexture> color;
    std::unique_ptr<plume::RenderTexture> depth;
    std::unique_ptr<plume::RenderFramebuffer> framebuffer;
    std::unique_ptr<plume::RenderCommandList> command_list;
    std::unique_ptr<plume::RenderCommandFence> fence;
    // A present submits its frame and returns while the GPU renders it: the
    // next frame records into the spare list, and waits for the frame in
    // flight only when it is submitted itself (SFR_GPU_PIPELINE=0: present
    // waits for its frame, as every flush does).
    std::unique_ptr<plume::RenderCommandList> spare_list;
    std::unique_ptr<plume::RenderCommandFence> spare_fence;
    bool spare_in_flight = false;
    static bool pipelined() {
        static const bool enabled = [] {
            const char* const text = std::getenv("SFR_GPU_PIPELINE");
            return !text || *text != '0';
        }();
        return enabled;
    }
    // Stretching present: one pipeline sampling the framebuffer into the
    // acquired swap-chain texture, built the first time it is needed.
    std::unique_ptr<plume::RenderShader> blit_vertex, blit_pixel;
    std::unique_ptr<plume::RenderPipelineLayout> blit_layout;
    std::unique_ptr<plume::RenderPipeline> blit_pipeline;
    // The last blit's viewport (x, y, width, height) and window size, for touches.
    std::array<float, 6> touch_view{};
#ifdef __ANDROID__
    // Android destroys the window's surface in the background; the swap chain
    // is made again on the new one when the app returns (or a present fails).
    bool rebuild_swap_chain = false;
    // Back held for a second leaves the game (a short press is the game's B).
    uint32_t back_down_ms = 0;
    bool rebuild_surface();
#endif
#ifndef _WIN32
    TouchControls touch_controls;
    SDL_Sensor* tilt_sensor = nullptr;
    bool tilt_sensor_checked = false;
    void read_touch_controls();
#endif
    std::unique_ptr<plume::RenderDescriptorSet> blit_texture, blit_sampler;
    std::unique_ptr<plume::RenderSampler> blit_sampler_object;
    std::unique_ptr<plume::RenderTextureView> blit_view;
    std::vector<std::unique_ptr<plume::RenderFramebuffer>> blit_targets;
    std::pair<uint32_t, uint32_t> presented_area{0, 0};
    void build_blit();
    void build_blit_targets();

    ~Impl() {
        if (ready && open) {
            // Unsubmitted commands may reference resources of owners already
            // destroyed; discard them instead of executing.
            command_list->end();
            open = false;
        }
        if (ready) {
            command_list->begin();
            command_list->end();
            graphics->queue().executeCommandLists(command_list.get(), fence.get());
            graphics->queue().waitForCommandFence(fence.get());
            if (spare_in_flight) graphics->queue().waitForCommandFence(spare_fence.get());
            spare_in_flight = false;
        }
        fence.reset();
        command_list.reset();
        spare_fence.reset();
        spare_list.reset();
        blit_targets.clear();
        blit_pipeline.reset();
        blit_layout.reset();
        blit_texture.reset();
        blit_sampler.reset();
        blit_sampler_object.reset();
        blit_view.reset();
        blit_vertex.reset();
        blit_pixel.reset();
        framebuffer.reset();
        depth.reset();
        color.reset();
#ifdef _WIN32
        // Plume's D3D12 swap chain leaves its waitable object open.
        if (swap_chain && graphics->backend() == sfr::GraphicsBackend::d3d12) {
            auto* native_swap_chain = static_cast<plume::D3D12SwapChain*>(swap_chain.get());
            if (native_swap_chain->waitableObject) {
                CloseHandle(native_swap_chain->waitableObject);
                native_swap_chain->waitableObject = nullptr;
            }
        }
#endif
        swap_chain.reset();
#ifdef _WIN32
        if (window) DestroyWindow(window);
#else
        if (window) SDL_HideWindow(window);
#endif
    }

    void wait_for(plume::RenderCommandFence* target) {
        const auto wait = [this, target] { graphics->queue().waitForCommandFence(target); };
        if (!gpu_wait()) { wait(); return; }
        in_gpu_wait.store(true, std::memory_order_release);
        try { gpu_wait()(wait); }
        catch (...) { in_gpu_wait.store(false, std::memory_order_release); throw; }
        in_gpu_wait.store(false, std::memory_order_release);
    }
    // Each submission's fence event is waited for exactly once.
    void wait_in_flight() {
        if (!spare_in_flight) return;
        spare_in_flight = false;
        wait_for(spare_fence.get());
    }
    // Vulkan: the list that draws into an acquired swap-chain image waits for
    // the acquisition and signals the presentation (D3D12 needs neither).
    std::vector<std::unique_ptr<plume::RenderCommandSemaphore>> acquired, rendered;
    uint32_t next_acquired = 0;
    plume::RenderCommandSemaphore* wait_semaphore = nullptr;
    plume::RenderCommandSemaphore* signal_semaphore = nullptr;
    void run_list() {
        const plume::RenderCommandList* lists[] = {command_list.get()};
        uint32_t waits = wait_semaphore ? 1 : 0, signals = signal_semaphore ? 1 : 0;
        graphics->queue().executeCommandLists(lists, 1, &wait_semaphore, waits, &signal_semaphore, signals, fence.get());
        wait_semaphore = signal_semaphore = nullptr;
    }
    void execute() {
        run_list();
        wait_for(fence.get());
        wait_in_flight();
    }
    // Submits without waiting; the frame in flight before it must finish
    // first, as its list becomes the one recorded next.
    void submit() {
        wait_in_flight();
        run_list();
        std::swap(command_list, spare_list);
        std::swap(fence, spare_fence);
        spare_in_flight = true;
    }
    static std::function<void(const std::function<void()>&)>& gpu_wait() {
        static std::function<void(const std::function<void()>&)> wrapper;
        return wrapper;
    }
    std::atomic<bool> in_gpu_wait{false};
    void refuse_during_gpu_wait() const {
        if (in_gpu_wait.load(std::memory_order_acquire))
            throw std::logic_error("native presentation used while it waits for the GPU");
    }
    // Draws and clears accumulate in one open list per frame; present,
    // readback and explicit flushes submit it.
    bool open = false;
    std::vector<std::function<void(bool)>> after_flush;
    void ensure_open() {
        if (open) return;
        command_list->begin();
        std::array<plume::RenderTextureBarrier, 2> barriers{
            plume::RenderTextureBarrier(color.get(), plume::RenderTextureLayout::COLOR_WRITE),
            plume::RenderTextureBarrier(depth.get(), plume::RenderTextureLayout::DEPTH_WRITE)};
        command_list->barriers(plume::RenderBarrierStage::GRAPHICS, barriers.data(), 2);
        command_list->setFramebuffer(framebuffer.get());
        open = true;
    }
    void flush() {
        if (open) {
            command_list->end();
            execute();
            open = false;
        } else {
            wait_in_flight();
        }
        for (auto& callback : after_flush) callback(true);
    }
};


void NativePresentation::Impl::build_blit() {
    if (blit_pipeline) return;
    auto& device = graphics->device();
    // src/shaders/blit.hlsl, compiled for this backend.
    if (graphics->backend() == sfr::GraphicsBackend::vulkan) {
        blit_vertex = device.createShader(blit_vs_spirv, sizeof(blit_vs_spirv), "vertexMain", plume::RenderShaderFormat::SPIRV);
        blit_pixel = device.createShader(blit_ps_spirv, sizeof(blit_ps_spirv), "pixelMain", plume::RenderShaderFormat::SPIRV);
    } else {
        blit_vertex = device.createShader(blit_vs_dxil, sizeof(blit_vs_dxil), "vertexMain", plume::RenderShaderFormat::DXIL);
        blit_pixel = device.createShader(blit_ps_dxil, sizeof(blit_ps_dxil), "pixelMain", plume::RenderShaderFormat::DXIL);
    }
    if (!blit_vertex || !blit_pixel) unavailable("blit shaders");

    plume::RenderPipelineLayoutBuilder layout;
    layout.begin(false, true);
    plume::RenderDescriptorSetBuilder textures;
    textures.begin();
    textures.addTexture(0);
    textures.end();
    blit_texture = textures.create(&device);
    layout.addDescriptorSet(textures);
    plume::RenderDescriptorSetBuilder samplers;
    samplers.begin();
    samplers.addSampler(0);
    samplers.end();
    blit_sampler = samplers.create(&device);
    layout.addDescriptorSet(samplers);
    // The sampled area and stick knob, then the touch controls' circles (blit.hlsl).
    layout.addPushConstant(0, 2, sizeof(float) * 32,
                           plume::RenderShaderStageFlag::VERTEX | plume::RenderShaderStageFlag::PIXEL);
    layout.end();
    blit_layout = layout.create(&device);
    if (!blit_layout || !blit_texture || !blit_sampler) unavailable("blit pipeline layout");

    plume::RenderSamplerDesc sampler_desc;
    sampler_desc.minFilter = plume::RenderFilter::LINEAR;
    sampler_desc.magFilter = plume::RenderFilter::LINEAR;
    sampler_desc.addressU = plume::RenderTextureAddressMode::CLAMP;
    sampler_desc.addressV = plume::RenderTextureAddressMode::CLAMP;
    sampler_desc.addressW = plume::RenderTextureAddressMode::CLAMP;
    blit_sampler_object = device.createSampler(sampler_desc);
    blit_view = color->createTextureView(
        plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::B8G8R8A8_UNORM));
    if (!blit_sampler_object || !blit_view) unavailable("blit sampler");
    blit_sampler->setSampler(0, blit_sampler_object.get());
    blit_texture->setTexture(0, color.get(), plume::RenderTextureLayout::SHADER_READ, blit_view.get());

    plume::RenderGraphicsPipelineDesc pipeline_desc;
    pipeline_desc.pipelineLayout = blit_layout.get();
    pipeline_desc.vertexShader = blit_vertex.get();
    pipeline_desc.pixelShader = blit_pixel.get();
    pipeline_desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    pipeline_desc.cullMode = plume::RenderCullMode::NONE;
    pipeline_desc.depthEnabled = false;
    pipeline_desc.depthWriteEnabled = false;
    pipeline_desc.renderTargetCount = 1;
    pipeline_desc.renderTargetFormat[0] = swap_chain_format;
    blit_pipeline = device.createGraphicsPipeline(pipeline_desc);
    if (!blit_pipeline) unavailable("blit pipeline");

    build_blit_targets();
}

void NativePresentation::Impl::build_blit_targets() {
    blit_targets.clear();
    for (uint32_t i = 0; i < swap_chain->getTextureCount(); ++i) {
        const plume::RenderTexture* attachment = swap_chain->getTexture(i);
        plume::RenderFramebufferDesc target(&attachment, 1);
        blit_targets.push_back(graphics->device().createFramebuffer(target));
        if (!blit_targets.back()) unavailable("blit render target");
    }
}

NativePresentation::NativePresentation(NativeGraphics& graphics, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0 || width > 8192 || height > 8192)
        throw std::invalid_argument("native presentation dimensions must be between 1 and 8192");

    auto implementation = std::make_unique<Impl>();
    implementation->graphics = &graphics;
    implementation->width = width;
    implementation->height = height;

    const bool d3d12 = graphics.backend() == GraphicsBackend::d3d12;
#ifdef _WIN32
    if (d3d12) {
        auto* native_device = static_cast<plume::D3D12Device*>(&graphics.device());
        auto* native_queue = static_cast<plume::D3D12CommandQueue*>(&graphics.queue());
        if (!native_device->d3d || !native_device->allocator || !native_queue->d3d)
            throw std::runtime_error("native graphics has unavailable D3D12 handles");
    }
#endif
    implementation->raster_state = std::make_unique<NativeRasterState>(graphics, width, height);

#ifdef _WIN32
    ensure_window_class();
#endif
    // The window (and swap chain) size is the player's; the game draws at
    // width x height and present scales it in (SFR_WINDOW_WIDTH/HEIGHT).
    const auto setting = [](const char* name, uint32_t fallback) {
        const char* text = std::getenv(name);
        const unsigned long value = text ? std::strtoul(text, nullptr, 10) : 0;
        return value >= 160 && value <= 16384 ? uint32_t(value) : fallback;
    };
    const uint32_t window_width = setting("SFR_WINDOW_WIDTH", width);
    const uint32_t window_height = setting("SFR_WINDOW_HEIGHT", height);
#ifdef _WIN32
    RECT bounds{0, 0, static_cast<LONG>(window_width), static_cast<LONG>(window_height)};
    constexpr DWORD style = windowed_style;
    if (!AdjustWindowRectEx(&bounds, style, FALSE, 0)) unavailable("window bounds");
    implementation->window = CreateWindowExW(
        0, window_class_name, L"Sonic Free Riders", style, CW_USEDEFAULT, CW_USEDEFAULT,
        bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, nullptr,
        GetModuleHandleW(nullptr), &implementation->close_requested);
#else
    // The window Plume's Vulkan instance was made for (NativeGraphics).
    implementation->window = static_cast<SDL_Window*>(graphics.host_window());
    if (implementation->window) {
        SDL_SetWindowSize(implementation->window, int(window_width), int(window_height));
        SDL_SetWindowPosition(implementation->window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
#endif
    if (!implementation->window) unavailable("window");

#ifdef __ANDROID__
    // Plume draws into the window's ANativeWindow on Android.
    SDL_SysWMinfo system{};
    SDL_VERSION(&system.version);
    if (!SDL_GetWindowWMInfo(implementation->window, &system) || !system.info.android.window) unavailable("native window");
    plume::RenderWindow render_window = system.info.android.window;
#else
    plume::RenderWindow render_window = implementation->window;
#endif
    implementation->swap_chain = graphics.queue().createSwapChain(
        render_window, 3, swap_chain_format, 2);
    if (!implementation->swap_chain) unavailable("swap chain wrapper");
#ifdef _WIN32
    if (d3d12) {
        auto* native_swap_chain = static_cast<plume::D3D12SwapChain*>(implementation->swap_chain.get());
        if (!native_swap_chain->d3d) {
            native_swap_chain->textureCount = static_cast<uint32_t>(native_swap_chain->textures.size());
            unavailable("D3D12 swap chain");
        }
        if (native_swap_chain->textures.size() != 3) unavailable("D3D12 swap chain");
        for (const auto& texture : native_swap_chain->textures)
            if (!texture.d3d) unavailable("D3D12 swap-chain texture");
    }
#endif
    if (!d3d12) {
        // Plume's Vulkan swap chain makes its images at the first resize.
        if (!implementation->swap_chain->resize()) unavailable("Vulkan swap chain images");
        for (uint32_t i = 0; i <= implementation->swap_chain->getTextureCount(); ++i) {
            implementation->acquired.push_back(graphics.device().createCommandSemaphore());
            implementation->rendered.push_back(graphics.device().createCommandSemaphore());
            if (!implementation->acquired.back() || !implementation->rendered.back()) unavailable("swap-chain semaphores");
        }
    }
    if (implementation->swap_chain->isEmpty() || implementation->swap_chain->getTextureCount() < 2)
        unavailable("swap chain");
    // Off unless asked for (Plume's Vulkan swap chain starts with it on).
    const char* vsync = std::getenv("SFR_VSYNC");
    implementation->swap_chain->setVsyncEnabled(vsync && *vsync == '1');
    if (const char* fullscreen = std::getenv("SFR_FULLSCREEN"); fullscreen && *fullscreen == '1')
        implementation->start_fullscreen = true;

    implementation->color = graphics.device().createTexture(
        plume::RenderTextureDesc::ColorTarget(width, height, plume::RenderFormat::B8G8R8A8_UNORM));
    implementation->depth = graphics.device().createTexture(
        plume::RenderTextureDesc::DepthTarget(width, height, plume::RenderFormat::D32_FLOAT_S8_UINT));
    if (!implementation->color) unavailable("color texture");
    if (!implementation->depth) unavailable("depth texture");
#ifdef _WIN32
    if (d3d12 && (!static_cast<plume::D3D12Texture*>(implementation->color.get())->d3d ||
                  !static_cast<plume::D3D12Texture*>(implementation->depth.get())->d3d))
        unavailable("D3D12 color or depth texture");
#endif

    const plume::RenderTexture* color_attachment = implementation->color.get();
    plume::RenderFramebufferDesc framebuffer_desc(&color_attachment, 1, implementation->depth.get());
    implementation->framebuffer = graphics.device().createFramebuffer(framebuffer_desc);
    implementation->command_list = graphics.queue().createCommandList();
    implementation->fence = graphics.device().createCommandFence();
    implementation->spare_list = graphics.queue().createCommandList();
    implementation->spare_fence = graphics.device().createCommandFence();
    if (!implementation->spare_list || !implementation->spare_fence) unavailable("spare command list");
    if (!implementation->framebuffer) unavailable("framebuffer");
    if (implementation->framebuffer->getWidth() != width || implementation->framebuffer->getHeight() != height)
        unavailable("framebuffer");
    if (!implementation->command_list) unavailable("command list");
    if (!implementation->fence) unavailable("command fence");
#ifdef _WIN32
    if (d3d12) {
        const auto* native_framebuffer = static_cast<plume::D3D12Framebuffer*>(implementation->framebuffer.get());
        if (native_framebuffer->colorHandles.size() != 1 || !native_framebuffer->colorHandles[0].ptr ||
            !native_framebuffer->depthHandle.ptr)
            unavailable("D3D12 framebuffer");
        const auto* native_commands = static_cast<plume::D3D12CommandList*>(implementation->command_list.get());
        if (!native_commands->d3d || !native_commands->commandAllocator) unavailable("command list");
        const auto* native_fence = static_cast<plume::D3D12CommandFence*>(implementation->fence.get());
        if (!native_fence->d3d || !native_fence->fenceEvent) unavailable("command fence");
    }
#endif
    implementation->ready = true;
    impl_ = std::move(implementation);
}

NativePresentation::~NativePresentation() = default;

uint32_t NativePresentation::width() const noexcept { return impl_->width; }
uint32_t NativePresentation::height() const noexcept { return impl_->height; }
plume::RenderTexture& NativePresentation::color() { return *impl_->color; }
plume::RenderTexture& NativePresentation::depth() { return *impl_->depth; }

void NativePresentation::clear(const NativeClear& clear, std::span<const plume::RenderRect> rectangles) {
    impl_->refuse_during_gpu_wait();
    if (clear.color)
        for (float component : clear.color_value)
            if (!std::isfinite(component)) throw std::invalid_argument("clear color must be finite");
    if ((clear.depth || clear.stencil) &&
        (!std::isfinite(clear.depth_value) || clear.depth_value < 0.0f || clear.depth_value > 1.0f))
        throw std::invalid_argument("clear depth must be finite and between zero and one");
    for (const auto& rect : rectangles)
        if (rect.left < 0 || rect.top < 0 || rect.left >= rect.right || rect.top >= rect.bottom ||
            static_cast<uint32_t>(rect.right) > impl_->width || static_cast<uint32_t>(rect.bottom) > impl_->height)
            throw std::invalid_argument("clear rectangle is empty or outside the presentation bounds");
    if (!clear.color && !clear.depth && !clear.stencil) return;

    impl_->ensure_open();
    impl_->raster_state->apply(*impl_->command_list);
    if (clear.color) {
        const auto& c = clear.color_value;
        impl_->command_list->clearColor(0, plume::RenderColor(c[0], c[1], c[2], c[3]), rectangles.data(), static_cast<uint32_t>(rectangles.size()));
    }
    if (clear.depth || clear.stencil)
        impl_->command_list->clearDepthStencil(clear.depth, clear.stencil, clear.depth_value, clear.stencil_value,
                                                rectangles.data(), static_cast<uint32_t>(rectangles.size()));
}

void NativePresentation::record(const std::function<void(plume::RenderCommandList&)>& body) {
    impl_->refuse_during_gpu_wait();
    impl_->ensure_open();
    impl_->raster_state->apply(*impl_->command_list);
    body(*impl_->command_list);
}

void NativePresentation::flush() { impl_->flush(); }

void NativePresentation::set_gpu_wait(std::function<void(const std::function<void()>&)> wait) {
    Impl::gpu_wait() = std::move(wait);
}

void NativePresentation::after_flush(std::function<void(bool)> callback) {
    impl_->after_flush.push_back(std::move(callback));
}

void NativePresentation::clear_after_flush() { impl_->after_flush.clear(); }

void NativePresentation::set_raster_state(const plume::RenderViewport& viewport, const plume::RenderRect& scissor) {
    // Applied by the next recorded draw or clear.
    impl_->raster_state->set(viewport, scissor);
}

const NativeRasterState& NativePresentation::raster_state() const noexcept { return *impl_->raster_state; }

std::pair<uint32_t, uint32_t> NativePresentation::presented_area() const noexcept {
    return impl_->presented_area.first ? impl_->presented_area : std::pair{impl_->width, impl_->height};
}

std::vector<uint8_t> NativePresentation::readback_color() {
    constexpr uint32_t bytes_per_pixel = 4;
    if (impl_->graphics->backend() == GraphicsBackend::vulkan) {
        // Plume copies buffers into textures only; the other way is native.
        const uint64_t row_pitch = uint64_t(impl_->width) * bytes_per_pixel;
        auto buffer = impl_->graphics->device().createBuffer(plume::RenderBufferDesc::ReadbackBuffer(row_pitch * impl_->height));
        if (!buffer) unavailable("readback buffer");
        impl_->flush();
        impl_->command_list->begin();
        impl_->command_list->barriers(plume::RenderBarrierStage::COPY,
            plume::RenderTextureBarrier(impl_->color.get(), plume::RenderTextureLayout::COPY_SOURCE));
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {impl_->width, impl_->height, 1};
        vkCmdCopyImageToBuffer(static_cast<plume::VulkanCommandList*>(impl_->command_list.get())->vk,
                               static_cast<plume::VulkanTexture*>(impl_->color.get())->vk,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               static_cast<plume::VulkanBuffer*>(buffer.get())->vk, 1, &region);
        impl_->command_list->barriers(plume::RenderBarrierStage::GRAPHICS,
            plume::RenderTextureBarrier(impl_->color.get(), plume::RenderTextureLayout::COLOR_WRITE));
        impl_->command_list->end();
        impl_->execute();
        const auto* mapped = static_cast<const uint8_t*>(buffer->map());
        if (!mapped) unavailable("mapped readback buffer");
        std::vector<uint8_t> result(mapped, mapped + row_pitch * impl_->height);
        buffer->unmap();
        return result;
    }
#ifndef _WIN32
    throw std::logic_error("native presentation readback: no D3D12 here");
#else
    const auto resource_desc = static_cast<plume::D3D12Texture*>(impl_->color.get())->d3d->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT row_count = 0;
    UINT64 row_bytes = 0;
    UINT64 total_bytes = 0;
    static_cast<plume::D3D12Device*>(&impl_->graphics->device())->d3d->GetCopyableFootprints(
        &resource_desc, 0, 1, 0, &footprint, &row_count, &row_bytes, &total_bytes);
    if (footprint.Footprint.Format != DXGI_FORMAT_B8G8R8A8_UNORM || row_count != impl_->height ||
        row_bytes != static_cast<UINT64>(impl_->width) * bytes_per_pixel ||
        footprint.Footprint.RowPitch < row_bytes || (footprint.Footprint.RowPitch & 255u) != 0)
        throw std::runtime_error("native presentation returned an invalid color copy footprint");
    auto buffer = impl_->graphics->device().createBuffer(
        plume::RenderBufferDesc::ReadbackBuffer(total_bytes));
    if (!buffer || !static_cast<plume::D3D12Buffer*>(buffer.get())->d3d) unavailable("readback buffer");

    impl_->flush();
    impl_->command_list->begin();
    impl_->command_list->barriers(plume::RenderBarrierStage::COPY,
        plume::RenderTextureBarrier(impl_->color.get(), plume::RenderTextureLayout::COPY_SOURCE));
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = static_cast<plume::D3D12Buffer*>(buffer.get())->d3d;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = static_cast<plume::D3D12Texture*>(impl_->color.get())->d3d;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    // Plume 1192686 dereferences a null destination texture for buffer footprints.
    static_cast<plume::D3D12CommandList*>(impl_->command_list.get())->d3d->CopyTextureRegion(
        &destination, 0, 0, 0, &source, nullptr);
    impl_->command_list->barriers(plume::RenderBarrierStage::GRAPHICS,
        plume::RenderTextureBarrier(impl_->color.get(), plume::RenderTextureLayout::COLOR_WRITE));
    impl_->command_list->end();
    impl_->execute();

    const auto* mapped = static_cast<const uint8_t*>(buffer->map());
    if (!mapped) unavailable("mapped readback buffer");
    std::vector<uint8_t> result(static_cast<std::size_t>(impl_->width) * impl_->height * bytes_per_pixel);
    for (uint32_t y = 0; y < impl_->height; ++y)
        std::memcpy(result.data() + static_cast<std::size_t>(y) * impl_->width * bytes_per_pixel,
                    mapped + footprint.Offset + static_cast<std::size_t>(y) * footprint.Footprint.RowPitch,
                    impl_->width * bytes_per_pixel);
    buffer->unmap();
    return result;
#endif
}

#ifdef __ANDROID__
bool NativePresentation::Impl::rebuild_surface() {
    flush();
    graphics->wait_idle([] { return false; });
    blit_targets.clear();
    swap_chain.reset();
    SDL_SysWMinfo system{};
    SDL_VERSION(&system.version);
    if (!SDL_GetWindowWMInfo(window, &system) || !system.info.android.window) return false;
    swap_chain = graphics->queue().createSwapChain(system.info.android.window, 3, swap_chain_format, 2);
    if (!swap_chain || !swap_chain->resize() || swap_chain->isEmpty()) {
        swap_chain.reset();
        return false;
    }
    const char* vsync = std::getenv("SFR_VSYNC");
    swap_chain->setVsyncEnabled(vsync && *vsync == '1');
    // A semaphore per image and one spare, as when the presentation was made.
    acquired.clear();
    rendered.clear();
    next_acquired = 0;
    for (uint32_t i = 0; i <= swap_chain->getTextureCount(); ++i) {
        acquired.push_back(graphics->device().createCommandSemaphore());
        rendered.push_back(graphics->device().createCommandSemaphore());
    }
    if (blit_pipeline) build_blit_targets();
    rebuild_swap_chain = false;
    std::cerr << "NATIVE_PRESENTATION surface rebuilt images=" << swap_chain->getTextureCount() << '\n';
    return true;
}
#endif

void NativePresentation::present(uint32_t area_width, uint32_t area_height) {
    impl_->refuse_during_gpu_wait();
#ifdef __ANDROID__
    // Back from the background: draw into the new surface, or skip the frame.
    if ((impl_->rebuild_swap_chain || !impl_->swap_chain) && !impl_->rebuild_surface()) return;
#endif
    // The window was resized (or went full screen): the swap chain follows
    // once nothing in flight still uses its textures.
    if (impl_->swap_chain->needsResize()) {
        impl_->flush();
        impl_->blit_targets.clear();
        if (!impl_->swap_chain->resize()) {
#ifdef __ANDROID__
            // The surface was lost (VK_ERROR_SURFACE_LOST_KHR on phones when
            // the window changes): make it again from the window next frame.
            impl_->rebuild_swap_chain = true;
            return;
#endif
            throw std::runtime_error("failed to resize the native swap chain");
        }
        if (impl_->blit_pipeline) impl_->build_blit_targets();
    }
    uint32_t texture_index = 0;
    plume::RenderCommandSemaphore* acquired = nullptr;
    plume::RenderCommandSemaphore* rendered = nullptr;
    if (!impl_->acquired.empty()) {
        acquired = impl_->acquired[impl_->next_acquired].get();
        impl_->next_acquired = (impl_->next_acquired + 1) % uint32_t(impl_->acquired.size());
    }
    if (!impl_->swap_chain->acquireTexture(acquired, &texture_index)) {
#ifdef __ANDROID__
        impl_->rebuild_swap_chain = true;  // the surface went away
        return;
#endif
        throw std::runtime_error("failed to acquire native presentation texture");
    }
    if (acquired) rendered = impl_->rendered[texture_index].get();
    auto* swap_texture = impl_->swap_chain->getTexture(texture_index);
    if (!swap_texture)
        throw std::runtime_error("acquired native presentation texture is unavailable");
#ifdef _WIN32
    if (impl_->graphics->backend() == GraphicsBackend::d3d12 && !static_cast<plume::D3D12Texture*>(swap_texture)->d3d)
        throw std::runtime_error("acquired native presentation texture is unavailable");
#endif

    const bool part = area_width && area_height && area_width <= impl_->width &&
                      area_height <= impl_->height && (area_width != impl_->width || area_height != impl_->height);
    impl_->presented_area = part ? std::pair{area_width, area_height} : std::pair{impl_->width, impl_->height};
    const auto [shown_width, shown_height] = impl_->presented_area;
    // Scaled into the window keeping its shape, the rest black; a straight
    // copy when the window is exactly the framebuffer.
    const uint32_t out_width = impl_->swap_chain->getWidth(), out_height = impl_->swap_chain->getHeight();
    const bool stretch = part || out_width != impl_->width || out_height != impl_->height;
    // The copy to the swap-chain texture follows the frame's draws in their
    // own list; pipelined, the frame is submitted without waiting for it.
    const bool pipelined = Impl::pipelined();
    if (pipelined) impl_->ensure_open();
    else {
        impl_->flush();
        impl_->command_list->begin();
    }
    if (stretch) {
        impl_->build_blit();
        const std::array<plume::RenderTextureBarrier, 2> draw_barriers{
            plume::RenderTextureBarrier(impl_->color.get(), plume::RenderTextureLayout::SHADER_READ),
            plume::RenderTextureBarrier(swap_texture, plume::RenderTextureLayout::COLOR_WRITE)};
        impl_->command_list->barriers(plume::RenderBarrierStage::GRAPHICS, draw_barriers.data(), 2);
        impl_->command_list->setFramebuffer(impl_->blit_targets[texture_index].get());
        impl_->command_list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
        const float scale = (std::min)(float(out_width) / float(shown_width), float(out_height) / float(shown_height));
        const float view_width = float(shown_width) * scale, view_height = float(shown_height) * scale;
        const plume::RenderViewport viewport((float(out_width) - view_width) * 0.5f, (float(out_height) - view_height) * 0.5f,
                                             view_width, view_height);
        const plume::RenderRect scissor(0, 0, int32_t(out_width), int32_t(out_height));
        impl_->command_list->setViewports(&viewport, 1);
        impl_->command_list->setScissors(&scissor, 1);
        impl_->command_list->setGraphicsPipelineLayout(impl_->blit_layout.get());
        impl_->command_list->setPipeline(impl_->blit_pipeline.get());
        impl_->command_list->setGraphicsDescriptorSet(impl_->blit_texture.get(), 0);
        impl_->command_list->setGraphicsDescriptorSet(impl_->blit_sampler.get(), 1);
        // The shown part of the framebuffer (all of it when the game names no
        // area: 0 here would sample its corner texel over the whole window).
        std::array<float, 32> constants{float(shown_width) / float(impl_->width),
                                        float(shown_height) / float(impl_->height)};
        if (touch_controls_enabled()) {
            const TouchOverlay overlay = touch_overlay();
            constants[2] = overlay.knob[0];
            constants[3] = overlay.knob[1];
            for (size_t i = 0; i < TouchOverlay::circles; ++i)
                for (size_t j = 0; j < 4; ++j) constants[4 + i * 4 + j] = overlay.circle[i][j];
        }
        impl_->touch_view = {viewport.x, viewport.y, viewport.width, viewport.height, float(out_width), float(out_height)};
        impl_->command_list->setGraphicsPushConstants(0, constants.data());
        impl_->command_list->drawInstanced(3, 1, 0, 0);
        impl_->command_list->setFramebuffer(nullptr);
    } else {
        const std::array<plume::RenderTextureBarrier, 2> copy_barriers{
            plume::RenderTextureBarrier(impl_->color.get(), plume::RenderTextureLayout::COPY_SOURCE),
            plume::RenderTextureBarrier(swap_texture, plume::RenderTextureLayout::COPY_DEST)};
        impl_->command_list->barriers(plume::RenderBarrierStage::COPY, copy_barriers.data(), 2);
        impl_->command_list->copyTexture(swap_texture, impl_->color.get());
    }
    impl_->command_list->barriers(plume::RenderBarrierStage::NONE,
        plume::RenderTextureBarrier(swap_texture, plume::RenderTextureLayout::PRESENT));
    impl_->command_list->barriers(plume::RenderBarrierStage::GRAPHICS,
        plume::RenderTextureBarrier(impl_->color.get(), plume::RenderTextureLayout::COLOR_WRITE));
    impl_->command_list->end();
    impl_->wait_semaphore = acquired;
    impl_->signal_semaphore = rendered;
    if (pipelined) {
        impl_->submit();
        impl_->open = false;
        for (auto& callback : impl_->after_flush) callback(false);
    } else {
        impl_->execute();
    }
    if (!impl_->swap_chain->present(texture_index, rendered ? &rendered : nullptr, rendered ? 1 : 0)) {
#ifdef __ANDROID__
        impl_->rebuild_swap_chain = true;
        return;
#endif
        throw std::runtime_error("failed to present native swap-chain texture");
    }
    if (!impl_->shown) {
#ifdef _WIN32
        ShowWindow(impl_->window, SW_SHOWNORMAL);
        UpdateWindow(impl_->window);
#else
        SDL_ShowWindow(impl_->window);
#endif
        if (impl_->start_fullscreen) set_fullscreen(impl_->window, true);
        impl_->shown = true;
    }
}

void NativePresentation::pump_events() {
#ifdef _WIN32
    MSG message{};
    while (PeekMessageW(&message, impl_->window, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
#else
    // SDL keeps keyboard and controller state from these events too.
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT ||
            (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE))
            impl_->close_requested = true;
        if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_RETURN && (event.key.keysym.mod & KMOD_ALT) &&
            !event.key.repeat)
            set_fullscreen(impl_->window, !(SDL_GetWindowFlags(impl_->window) & SDL_WINDOW_FULLSCREEN_DESKTOP));
#ifdef __ANDROID__
        // SDL holds this pump while the app is paused; back in front, the
        // window has a new surface.
        if (event.type == SDL_APP_DIDENTERFOREGROUND) impl_->rebuild_swap_chain = true;
        if (event.type == SDL_KEYDOWN && event.key.keysym.scancode == SDL_SCANCODE_AC_BACK) {
            if (!event.key.repeat) impl_->back_down_ms = SDL_GetTicks();
            else impl_->close_requested = true;  // Android repeats a long press
        }
        if (event.type == SDL_KEYUP && event.key.keysym.scancode == SDL_SCANCODE_AC_BACK) impl_->back_down_ms = 0;
#endif
    }
    if (touch_controls_enabled()) impl_->read_touch_controls();
#ifdef __ANDROID__
    if (impl_->back_down_ms && SDL_GetTicks() - impl_->back_down_ms >= 1000) {
        impl_->back_down_ms = 0;
        impl_->close_requested = true;  // back to the launcher
    }
#endif
#endif
}

#ifndef _WIN32
// Touches in window coordinates become game-image coordinates through the
// last blit's viewport; the accelerometer, when there is one, steers.
void NativePresentation::Impl::read_touch_controls() {
    const auto [view_x, view_y, view_width, view_height, out_width, out_height] = touch_view;
    std::vector<TouchPoint> touches;
    if (view_width > 0.0f && view_height > 0.0f) {
        for (int device = 0; device < SDL_GetNumTouchDevices(); ++device) {
            const SDL_TouchID id = SDL_GetTouchDevice(device);
            for (int index = 0; index < SDL_GetNumTouchFingers(id); ++index) {
                const SDL_Finger* finger = SDL_GetTouchFinger(id, index);
                if (!finger) continue;
                touches.push_back({int64_t(finger->id),
                                   (finger->x * out_width - view_x) / view_width,
                                   (finger->y * out_height - view_y) / view_height});
            }
        }
    }
    float tilt = 0.0f;
    if (!tilt_sensor_checked) {
        tilt_sensor_checked = true;
        const char* text = std::getenv("SFR_TILT");
        if ((!text || *text != '0') && SDL_InitSubSystem(SDL_INIT_SENSOR) == 0) {
            for (int i = 0; i < SDL_NumSensors() && !tilt_sensor; ++i)
                if (SDL_SensorGetDeviceType(i) == SDL_SENSOR_ACCEL) tilt_sensor = SDL_SensorOpen(i);
        }
    }
    if (tilt_sensor) {
        SDL_SensorUpdate();
        float acceleration[3]{};
        if (SDL_SensorGetData(tilt_sensor, acceleration, 3) == 0) {
            // The sensor reads in the device's portrait axes. Held in landscape
            // (its right edge up) and turned clockwise like a wheel, the reading
            // moves towards +y; flipped landscape mirrors that. Full lock at
            // 25 degrees, nothing below 5.
            const float magnitude = std::sqrt(acceleration[0] * acceleration[0] + acceleration[1] * acceleration[1] +
                                              acceleration[2] * acceleration[2]);
            const SDL_DisplayOrientation orientation = SDL_GetDisplayOrientation(0);
            const float side = orientation == SDL_ORIENTATION_LANDSCAPE_FLIPPED ? -1.0f : 1.0f;
            if (magnitude > 1.0f && (orientation == SDL_ORIENTATION_LANDSCAPE || orientation == SDL_ORIENTATION_LANDSCAPE_FLIPPED)) {
                const float angle = std::asin(std::clamp(side * acceleration[1] / magnitude, -1.0f, 1.0f)) * 57.29578f;
                const float beyond = std::max(std::abs(angle) - 5.0f, 0.0f) / 20.0f;
                tilt = std::copysign(std::min(beyond, 1.0f), angle);
            }
        }
    }
    touch_controls.update(touches, tilt, touch_racing());
    publish_touch_controls(touch_controls.state(), touch_controls.overlay());
}
#endif

bool NativePresentation::close_requested() const noexcept { return impl_->close_requested; }
void* NativePresentation::window_handle() const noexcept { return impl_->window; }
}
