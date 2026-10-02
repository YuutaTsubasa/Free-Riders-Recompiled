#include "avatar_transform.h"
#include "native_presentation.h"
#include <functional>

#include "native_graphics.h"
#include "native_raster_state.h"
#include "gltf_model.h"
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
#if defined(__ANDROID__) || defined(__APPLE__)
#include <SDL_syswm.h>
#endif
#ifdef __APPLE__
#include <SDL_metal.h>
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
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <string>
#include <iostream>

// The blit shaders, compiled at build time (CMakeLists.txt, sfr_embed_shader).
#include "blit_vs_dxil.h"
#include "model_vs_dxil.h"
#include "model_ps_dxil.h"
#include "blit_ps_dxil.h"
#include "blit_vs_spirv.h"
#include "blit_ps_spirv.h"
#include "model_vs_spirv.h"
#include "model_ps_spirv.h"

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
    uint32_t render_width = 0;
    uint32_t render_height = 0;
    plume::RenderRect render_rectangle(const plume::RenderRect& rect) const {
        // Round shared endpoints identically: neighboring player scissors and
        // clears must neither overlap nor leave a seam at fractional scales.
        const auto coordinate = [](int32_t value, uint32_t render, uint32_t logical) {
            return int32_t((uint64_t(value) * render + logical / 2) / logical);
        };
        return {coordinate(rect.left, render_width, width), coordinate(rect.top, render_height, height),
                coordinate(rect.right, render_width, width), coordinate(rect.bottom, render_height, height)};
    }
    // The title's viewport and scissor at the render resolution.
    void scaled_raster(plume::RenderViewport& viewport, plume::RenderRect& scissor) const {
        viewport = raster_state->viewport();
        const float x = float(render_width) / width, y = float(render_height) / height;
        viewport.x *= x; viewport.width *= x;
        viewport.y *= y; viewport.height *= y;
        scissor = render_rectangle(raster_state->scissor());
    }
    void apply_raster() {
        plume::RenderViewport viewport;
        plume::RenderRect scissor;
        scaled_raster(viewport, scissor);
        command_list->setViewports(viewport);
        command_list->setScissors(scissor);
    }
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
    // The player's own model, drawn over the frame while they race as the
    // Avatar (SFR_AVATAR_MODEL). Topology is static; posed vertices alternate
    // between two buffers while the preceding frame is on the GPU.
    std::optional<sfr::GltfModel> model;
    std::unique_ptr<plume::RenderShader> model_vertex, model_pixel;
    std::unique_ptr<plume::RenderPipelineLayout> model_layout;
    std::unique_ptr<plume::RenderPipeline> model_pipeline, model_double_sided_pipeline;
    std::unique_ptr<plume::RenderPipeline> model_reverse_pipeline, model_reverse_double_sided_pipeline;
    // One upload buffer per draw, recycled only after the preceding frame's
    // submission has completed. Both split-screen views can draw both riders.
    std::array<std::vector<std::unique_ptr<plume::RenderBuffer>>, 2> model_vertices;
    uint64_t model_present = ~uint64_t(0);
    size_t model_draw_index = 0;
    std::array<std::unique_ptr<plume::RenderBuffer>, 2> model_indices;
    uint32_t model_vertex_slot = 0;
    bool model_animation_enabled = true;
    bool model_mirrored = false;
    // Each part's slice of the index buffer, the picture its material paints
    // it with and the colour that picture is multiplied by: one draw a part,
    // so a face is not the colour of a sleeve.
    struct ModelPart {
        uint32_t first = 0, count = 0, picture = 0;
        float colour[4] = {1, 1, 1, 1};
        float cutoff = 0;
        bool double_sided = false, unlit = false;
    };
    std::vector<ModelPart> model_parts;
    // The model's pictures, and last of them a single white pixel for the
    // parts that have none, so that one pipeline draws both.
    std::vector<std::unique_ptr<plume::RenderTexture>> model_pictures;
    std::vector<std::unique_ptr<plume::RenderTextureView>> model_picture_views;
    std::vector<std::unique_ptr<plume::RenderDescriptorSet>> model_picture_sets;
    std::unique_ptr<plume::RenderDescriptorSet> model_sampler;
    std::unique_ptr<plume::RenderSampler> model_sampler_object;
    uint32_t model_index_count = 0;
    bool model_tried = false;
    void build_model();
    void draw_model(const AvatarFrameTransform& frame);
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
        try { drain(); } catch (...) {}
        stop_render_thread();
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
        static const bool fast = [] {
            const char* text = std::getenv("SFR_GPU_WAIT_FAST");
            return !text || *text != '0';
        }();
        if (fast) {
#ifdef _WIN32
            if (graphics->backend() == sfr::GraphicsBackend::d3d12) {
                // Plume uses an auto-reset event. This zero-time wait also
                // consumes it; waiting a second time would block forever.
                const auto* fence = static_cast<plume::D3D12CommandFence*>(target);
                if (WaitForSingleObjectEx(fence->fenceEvent, 0, FALSE) == WAIT_OBJECT_0) return;
            } else
#endif
            {
                const auto* fence = static_cast<plume::VulkanCommandFence*>(target);
                if (vkGetFenceStatus(fence->device->vk, fence->vk) == VK_SUCCESS) {
                    // Unlike the D3D12 event, querying does not consume the
                    // signal. Plume's wait resets this fence before reuse.
                    wait();
                    return;
                }
            }
        }
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
    void wait_presentation_queue() {
        if (graphics->backend() != GraphicsBackend::vulkan) return;
        auto* queue = static_cast<plume::VulkanCommandQueue*>(&graphics->queue());
        const std::scoped_lock lock(*queue->queue->mutex);
        if (vkQueueWaitIdle(queue->queue->vk) != VK_SUCCESS)
            throw std::runtime_error("failed to wait for the presentation queue before resizing");
    }
    void rebuild_swap_chain_sync() {
        if (graphics->backend() != GraphicsBackend::vulkan) return;
        // Image count can grow after resize (observed on Adreno). Never index
        // the old image-indexed signals with the new swap chain's image index.
        // Allocate first, so an allocation failure leaves the old set intact.
        decltype(acquired) next_waits, next_signals;
        for (uint32_t i = 0; i <= swap_chain->getTextureCount(); ++i) {
            next_waits.push_back(graphics->device().createCommandSemaphore());
            next_signals.push_back(graphics->device().createCommandSemaphore());
            if (!next_waits.back() || !next_signals.back() ||
                !static_cast<plume::VulkanCommandSemaphore*>(next_waits.back().get())->vk ||
                !static_cast<plume::VulkanCommandSemaphore*>(next_signals.back().get())->vk)
                unavailable("swap-chain semaphores");
        }
        acquired = std::move(next_waits);
        rendered = std::move(next_signals);
        next_acquired = 0;
        wait_semaphore = signal_semaphore = nullptr;
    }
    void run_list() {
        drain();
        for (auto& callback : before_submit) callback();
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
    // The render thread: draws are recorded into the open command list by a
    // thread of its own (NativePresentation::record_async), so the guest's
    // thread only queues them. One producer, one consumer; the producer
    // empties the queue (drain) before it touches the list itself.
    // SFR_RENDER_THREAD=1 forces it on, =0 off. Unset, it is on for D3D12, where
    // it was measured, and off elsewhere until another backend has been.
    bool render_thread_enabled() const {
        static const int choice = [] {
            const char* const text = std::getenv("SFR_RENDER_THREAD");
            return text && *text ? (*text != '0' ? 1 : 0) : -1;
        }();
        return choice < 0 ? graphics->backend() == sfr::GraphicsBackend::d3d12 : choice == 1;
    }
    static constexpr uint64_t queue_capacity = 4096;
    struct RecordSlot {
        NativePresentation::RecordFunction function = nullptr;
        plume::RenderViewport viewport;  // as the title had them when the draw was asked for
        plume::RenderRect scissor;
        uint64_t generation = 0;
        alignas(16) unsigned char payload[NativePresentation::record_payload_bytes];
    };
    std::unique_ptr<RecordSlot[]> slots;
    std::atomic<uint64_t> queue_head{0}, queue_tail{0};  // consumed, produced
    std::atomic<bool> worker_sleeping{false}, producer_waiting{false}, worker_stop{false}, worker_failed{false};
    std::mutex queue_mutex;
    std::condition_variable work_ready, work_done;
    std::exception_ptr worker_error;
    std::thread render_thread;

    void start_render_thread() {
        slots = std::make_unique<RecordSlot[]>(queue_capacity);
        render_thread = std::thread([this] { run_render_thread(); });
    }
    void run_render_thread() {
        for (;;) {
            const uint64_t head = queue_head.load(std::memory_order_relaxed);
            if (head != queue_tail.load(std::memory_order_seq_cst)) {
                auto& slot = slots[head % queue_capacity];
                if (!worker_failed.load(std::memory_order_relaxed)) {
                    try {
                        command_list->setViewports(slot.viewport);
                        command_list->setScissors(slot.scissor);
                        slot.function(slot.payload, *command_list, slot.generation);
                    } catch (...) {
                        std::lock_guard lock(queue_mutex);
                        worker_error = std::current_exception();
                        worker_failed.store(true, std::memory_order_release);
                    }
                }
                queue_head.store(head + 1, std::memory_order_seq_cst);
                if (producer_waiting.load(std::memory_order_seq_cst)) {
                    std::lock_guard lock(queue_mutex);
                    work_done.notify_one();
                }
                continue;
            }
            // Empty: spin a little (the next draw is usually microseconds
            // away), then sleep until the producer wakes the thread.
            bool found = false;
            for (int i = 0; i < 2000 && !found; ++i) {
                found = head != queue_tail.load(std::memory_order_acquire);
                if (!found) std::this_thread::yield();
            }
            if (found) continue;
            std::unique_lock lock(queue_mutex);
            worker_sleeping.store(true, std::memory_order_seq_cst);
            work_ready.wait(lock, [&] {
                return worker_stop.load(std::memory_order_seq_cst) ||
                       head != queue_tail.load(std::memory_order_seq_cst);
            });
            worker_sleeping.store(false, std::memory_order_seq_cst);
            if (worker_stop.load(std::memory_order_seq_cst) && head == queue_tail.load(std::memory_order_seq_cst)) return;
        }
    }
    // Waits until the render thread has consumed entry target - 1.
    void wait_for_head(uint64_t target) {
        for (int i = 0; i < 4000 && queue_head.load(std::memory_order_acquire) < target; ++i)
            std::this_thread::yield();
        if (queue_head.load(std::memory_order_seq_cst) >= target) return;
        std::unique_lock lock(queue_mutex);
        producer_waiting.store(true, std::memory_order_seq_cst);
        work_done.wait(lock, [&] { return queue_head.load(std::memory_order_seq_cst) >= target; });
        producer_waiting.store(false, std::memory_order_seq_cst);
    }
    void rethrow_worker_error() {
        if (!worker_failed.load(std::memory_order_acquire)) return;
        std::exception_ptr error;
        {
            std::lock_guard lock(queue_mutex);
            error = std::exchange(worker_error, nullptr);
            worker_failed.store(false, std::memory_order_release);
        }
        if (error) std::rethrow_exception(error);
    }
    // Every command asked for so far is in the list.
    void drain() {
        if (!render_thread.joinable()) return;
        wait_for_head(queue_tail.load(std::memory_order_relaxed));
        rethrow_worker_error();
    }
    void stop_render_thread() {
        if (!render_thread.joinable()) return;
        {
            std::lock_guard lock(queue_mutex);
            worker_stop.store(true, std::memory_order_seq_cst);
        }
        work_ready.notify_one();
        render_thread.join();
    }
    void enqueue(NativePresentation::RecordFunction function, const void* payload, size_t bytes) {
        if (!render_thread_enabled()) {
            ensure_open();
            apply_raster();
            function(payload, *command_list, list_generation);
            return;
        }
        ensure_open();
        rethrow_worker_error();
        if (!render_thread.joinable()) start_render_thread();
        const uint64_t tail = queue_tail.load(std::memory_order_relaxed);
        if (tail - queue_head.load(std::memory_order_acquire) >= queue_capacity) wait_for_head(tail - queue_capacity + 1);
        auto& slot = slots[tail % queue_capacity];
        slot.function = function;
        scaled_raster(slot.viewport, slot.scissor);
        slot.generation = list_generation;
        std::memcpy(slot.payload, payload, bytes);
        queue_tail.store(tail + 1, std::memory_order_seq_cst);
        if (worker_sleeping.load(std::memory_order_seq_cst)) {
            std::lock_guard lock(queue_mutex);
            work_ready.notify_one();
        }
    }
    // Draws and clears accumulate in one open list per frame; present,
    // readback and explicit flushes submit it.
    bool open = false;
    std::vector<std::function<void()>> before_submit;
    std::vector<std::function<void(bool)>> after_flush;
    // Binding generation: new lists and custom passes both invalidate the
    // layout/pipeline/descriptors cached by NativeRenderer.
    uint64_t list_generation = 0;
    void begin_list() {
        drain();
        command_list->begin();
        ++list_generation;
    }
    void ensure_open() {
        if (open) return;
        begin_list();
        std::array<plume::RenderTextureBarrier, 2> barriers{
            plume::RenderTextureBarrier(color.get(), plume::RenderTextureLayout::COLOR_WRITE),
            plume::RenderTextureBarrier(depth.get(), plume::RenderTextureLayout::DEPTH_WRITE)};
        command_list->barriers(plume::RenderBarrierStage::GRAPHICS, barriers.data(), 2);
        command_list->setFramebuffer(framebuffer.get());
        open = true;
    }
    void flush() {
        drain();
        if (open) {
            command_list->end();
            execute();
            open = false;
        } else {
            for (auto& callback : before_submit) callback();
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
    // A blend description of its own: the default one leaves every factor
    // UNKNOWN, which reaches Vulkan as VK_BLEND_FACTOR_MAX_ENUM and is not a
    // valid value even with blending off (the validation layer reports six of
    // them for this pipeline). This one writes the source, as the blit wants.
    pipeline_desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
    blit_pipeline = device.createGraphicsPipeline(pipeline_desc);
    if (!blit_pipeline) unavailable("blit pipeline");

    build_blit_targets();
}

// The model the player chose for the Avatar, uploaded once. SFR_AVATAR_MODEL
// names a .vrm (or any binary glTF); without it nothing is drawn and nothing
// is built.
void NativePresentation::Impl::build_model() {
    if (model_tried) return;
    model_tried = true;
    const auto path = sfr::avatar_model_path();
    if (path.empty()) return;
    // Standing as a rider, not in the T-pose the file was authored in;
    // SFR_AVATAR_MODEL_POSE=rest asks for the file as it is, which is how to
    // tell a posing mistake from a modelling one.
    const char* const pose_text = std::getenv("SFR_AVATAR_MODEL_POSE");
    const bool rest_pose = pose_text && std::strcmp(pose_text, "rest") == 0;
    model_animation_enabled = !rest_pose;
    std::string why;
    model = sfr::load_binary_gltf(path, &why, rest_pose ? sfr::GltfPose::rest : sfr::GltfPose::riding);
    if (!model) {
        const auto utf8 = path.u8string();
        std::cerr << "NATIVE_MODEL unavailable=" << why << " path=" << std::string(utf8.begin(), utf8.end()) << '\n';
        return;
    }
    // One buffer for the lot: position and normal a vertex, with each
    // primitive's indices shifted past the vertices before it.
    std::vector<float> vertices;
    std::vector<uint32_t> indices;
    for (const auto& primitive : model->primitives) {
        // Where this part's vertices start, counted in vertices: eight floats
        // each, position, normal and texture coordinate.
        const uint32_t base = uint32_t(vertices.size() / 8);
        const size_t count = primitive.positions.size() / 3;
        const bool shaded = primitive.normals.size() == primitive.positions.size();
        ModelPart part;
        part.first = uint32_t(indices.size());
        part.count = uint32_t(primitive.indices.size());
        for (int channel = 0; channel < 4; ++channel) part.colour[channel] = primitive.colour[channel];
        // The last picture is the white one, which is what a part with no
        // picture of its own is painted with.
        part.picture = primitive.image < model->images.size() ? primitive.image : uint32_t(model->images.size());
        part.cutoff = primitive.alpha_cutoff;
        part.double_sided = primitive.double_sided;
        part.unlit = primitive.unlit;
        model_parts.push_back(part);
        for (size_t vertex = 0; vertex < count; ++vertex) {
            vertices.push_back(primitive.positions[vertex * 3]);
            vertices.push_back(primitive.positions[vertex * 3 + 1]);
            vertices.push_back(primitive.positions[vertex * 3 + 2]);
            vertices.push_back(shaded ? primitive.normals[vertex * 3] : 0.0f);
            vertices.push_back(shaded ? primitive.normals[vertex * 3 + 1] : 1.0f);
            vertices.push_back(shaded ? primitive.normals[vertex * 3 + 2] : 0.0f);
            const bool painted = primitive.texcoords.size() == primitive.positions.size() / 3 * 2;
            vertices.push_back(painted ? primitive.texcoords[vertex * 2] : 0.0f);
            vertices.push_back(painted ? primitive.texcoords[vertex * 2 + 1] : 0.0f);
        }
        for (const uint32_t index : primitive.indices) indices.push_back(base + index);
    }
    model_index_count = uint32_t(indices.size());
    if (!model_index_count) { model.reset(); return; }

    auto& device = graphics->device();
    const uint64_t vertex_bytes = vertices.size() * sizeof(float);
    const uint64_t index_bytes = indices.size() * sizeof(uint32_t);
    for (auto& group : model_vertices) {
        group.push_back(device.createBuffer(plume::RenderBufferDesc::UploadBuffer(vertex_bytes, plume::RenderBufferFlag::VERTEX)));
        if (!group.back()) { model.reset(); return; }
    }
    for (auto& buffer : model_indices)
        buffer = device.createBuffer(plume::RenderBufferDesc::UploadBuffer(index_bytes, plume::RenderBufferFlag::INDEX));
    if (!model_indices[0] || !model_indices[1]) { model.reset(); return; }
    // A reflected skeleton reverses winding. Keep both static index orders so
    // back-face culling and SV_IsFrontFace agree with the reflected normals.
    for (auto& buffer : model_indices) {
        void* const at = buffer->map();
        if (!at) { model.reset(); return; }
        std::memcpy(at, indices.data(), size_t(index_bytes));
        buffer->unmap();
        for (size_t i = 0; i + 2 < indices.size(); i += 3) std::swap(indices[i + 1], indices[i + 2]);
    }

    if (graphics->backend() == sfr::GraphicsBackend::vulkan) {
        model_vertex = device.createShader(model_vs_spirv, sizeof(model_vs_spirv), "vertexMain", plume::RenderShaderFormat::SPIRV);
        model_pixel = device.createShader(model_ps_spirv, sizeof(model_ps_spirv), "pixelMain", plume::RenderShaderFormat::SPIRV);
    } else {
        model_vertex = device.createShader(model_vs_dxil, sizeof(model_vs_dxil), "vertexMain", plume::RenderShaderFormat::DXIL);
        model_pixel = device.createShader(model_ps_dxil, sizeof(model_ps_dxil), "pixelMain", plume::RenderShaderFormat::DXIL);
    }
    plume::RenderPipelineLayoutBuilder layout;
    layout.begin(false, true);
    plume::RenderDescriptorSetBuilder pictures;
    pictures.begin();
    pictures.addTexture(0);
    pictures.end();
    layout.addDescriptorSet(pictures);
    plume::RenderDescriptorSetBuilder samplers;
    samplers.begin();
    samplers.addSampler(0);
    samplers.end();
    model_sampler = samplers.create(&device);
    layout.addDescriptorSet(samplers);
    layout.addPushConstant(0, 2, sizeof(float) * 28,
                           plume::RenderShaderStageFlag::VERTEX | plume::RenderShaderStageFlag::PIXEL);
    layout.end();
    model_layout = layout.create(&device);
    if (!model_vertex || !model_pixel || !model_layout || !model_sampler) { model.reset(); return; }

    plume::RenderSamplerDesc sampler_desc;
    sampler_desc.minFilter = plume::RenderFilter::LINEAR;
    sampler_desc.magFilter = plume::RenderFilter::LINEAR;
    // A VRM's texture coordinates run past the edge where a part is meant to
    // repeat, which is what its own sampler asks for.
    sampler_desc.addressU = plume::RenderTextureAddressMode::WRAP;
    sampler_desc.addressV = plume::RenderTextureAddressMode::WRAP;
    sampler_desc.addressW = plume::RenderTextureAddressMode::WRAP;
    model_sampler_object = device.createSampler(sampler_desc);
    if (!model_sampler_object) { model.reset(); return; }
    model_sampler->setSampler(0, model_sampler_object.get());

    // The pictures, uploaded once. This waits for the copy: it happens at
    // load, and the alternative is keeping the staging buffers alive for a
    // frame to save a wait nobody sees.
    {
        auto upload = graphics->queue().createCommandList();
        auto finished = device.createCommandFence();
        std::vector<std::unique_ptr<plume::RenderBuffer>> staging;
        if (!upload || !finished) { model.reset(); return; }
        sfr::DecodedImage white;
        white.width = white.height = 1;
        white.rgba = {255, 255, 255, 255};
        upload->begin();
        for (size_t index = 0; index <= model->images.size(); ++index) {
            const sfr::DecodedImage& picture = index < model->images.size() ? model->images[index] : white;
            auto texture = device.createTexture(plume::RenderTextureDesc::Texture2D(
                picture.width, picture.height, 1, plume::RenderFormat::R8G8B8A8_UNORM));
            // A row of a texture being copied starts on a 256 byte boundary.
            const uint32_t row_bytes = (picture.width * 4 + 255) & ~255u;
            auto buffer = device.createBuffer(
                plume::RenderBufferDesc::UploadBuffer(uint64_t(row_bytes) * picture.height));
            if (!texture || !buffer) { model.reset(); return; }
            if (auto* const at = static_cast<uint8_t*>(buffer->map())) {
                for (uint32_t row = 0; row < picture.height; ++row)
                    std::memcpy(at + uint64_t(row) * row_bytes, picture.rgba.data() + size_t(row) * picture.width * 4,
                                size_t(picture.width) * 4);
                buffer->unmap();
            }
            upload->barriers(plume::RenderBarrierStage::COPY,
                             plume::RenderTextureBarrier(texture.get(), plume::RenderTextureLayout::COPY_DEST));
            upload->copyTextureRegion(
                plume::RenderTextureCopyLocation::Subresource(texture.get()),
                plume::RenderTextureCopyLocation::PlacedFootprint(buffer.get(), plume::RenderFormat::R8G8B8A8_UNORM,
                                                                  picture.width, picture.height, 1, row_bytes / 4));
            upload->barriers(plume::RenderBarrierStage::GRAPHICS,
                             plume::RenderTextureBarrier(texture.get(), plume::RenderTextureLayout::SHADER_READ));
            auto view = texture->createTextureView(
                plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R8G8B8A8_UNORM));
            auto set = pictures.create(&device);
            if (!view || !set) { model.reset(); return; }
            set->setTexture(0, texture.get(), plume::RenderTextureLayout::SHADER_READ, view.get());
            model_pictures.push_back(std::move(texture));
            model_picture_views.push_back(std::move(view));
            model_picture_sets.push_back(std::move(set));
            staging.push_back(std::move(buffer));
        }
        upload->end();
        graphics->queue().executeCommandLists(upload.get(), finished.get());
        graphics->queue().waitForCommandFence(finished.get());
    }

    const plume::RenderInputElement elements[3] = {
        plume::RenderInputElement("POSITION", 0, 0, plume::RenderFormat::R32G32B32_FLOAT, 0, 0),
        plume::RenderInputElement("NORMAL", 0, 1, plume::RenderFormat::R32G32B32_FLOAT, 0, 12),
        plume::RenderInputElement("TEXCOORD", 0, 2, plume::RenderFormat::R32G32_FLOAT, 0, 24)};
    const plume::RenderInputSlot slot(0, sizeof(float) * 8);
    plume::RenderGraphicsPipelineDesc pipeline_desc;
    pipeline_desc.pipelineLayout = model_layout.get();
    pipeline_desc.vertexShader = model_vertex.get();
    pipeline_desc.pixelShader = model_pixel.get();
    pipeline_desc.inputElements = elements;
    pipeline_desc.inputElementsCount = 3;
    pipeline_desc.inputSlots = &slot;
    pipeline_desc.inputSlotsCount = 1;
    pipeline_desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    // Back faces are dropped, so the model's own far side does not draw over
    // its near one: glTF winds its front faces counter-clockwise.
    // SFR_AVATAR_MODEL_FLAT keeps them, for telling a winding problem from a
    // placement one.
    static const bool flat = [] {
        const char* const text = std::getenv("SFR_AVATAR_MODEL_FLAT");
        return text && *text && *text != '0';
    }();
    pipeline_desc.cullMode = flat ? plume::RenderCullMode::NONE : plume::RenderCullMode::BACK;
    // Share the scene depth so later scenery cannot overwrite the rider.
    // The title uses reversed depth in races; normal viewports are also valid.
    static const bool flatten_depth = [] {
        const char* const text = std::getenv("SFR_AVATAR_MODEL_NO_DEPTH");
        return text && *text && *text != '0';
    }();
    pipeline_desc.depthEnabled = !flatten_depth;
    pipeline_desc.depthWriteEnabled = !flatten_depth;
    // Nearer is smaller: this projection puts the near plane at zero and
    // everything beyond it between there and one.
    pipeline_desc.depthFunction = plume::RenderComparisonFunction::LESS;
    pipeline_desc.renderTargetCount = 1;
    // The game's own frame, not the swap chain: its colour and depth formats.
    pipeline_desc.renderTargetFormat[0] = plume::RenderFormat::B8G8R8A8_UNORM;
    pipeline_desc.depthTargetFormat = plume::RenderFormat::D32_FLOAT_S8_UINT;
    pipeline_desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
    model_pipeline = device.createGraphicsPipeline(pipeline_desc);
    pipeline_desc.cullMode = plume::RenderCullMode::NONE;
    model_double_sided_pipeline = device.createGraphicsPipeline(pipeline_desc);
    pipeline_desc.depthFunction = plume::RenderComparisonFunction::GREATER;
    model_reverse_double_sided_pipeline = device.createGraphicsPipeline(pipeline_desc);
    pipeline_desc.cullMode = flat ? plume::RenderCullMode::NONE : plume::RenderCullMode::BACK;
    model_reverse_pipeline = device.createGraphicsPipeline(pipeline_desc);
    if (!model_pipeline || !model_double_sided_pipeline || !model_reverse_pipeline || !model_reverse_double_sided_pipeline) {
        model_pipeline.reset(); model.reset(); return;
    }
    std::cerr << "NATIVE_MODEL loaded primitives=" << model->primitives.size() << " vertices=" << model->vertices
              << " triangles=" << model->triangles << " height=" << (model->highest[1] - model->lowest[1])
              << " pictures=" << model->images.size() << '\n';
}

// Use this frame's actual Avatar world and camera transforms.
void NativePresentation::Impl::draw_model(const AvatarFrameTransform& frame) {
    if (frame.present != model_present) {
        model_present = frame.present;
        model_vertex_slot ^= 1;
        model_draw_index = 0;
    }
    auto& group = model_vertices[model_vertex_slot];
    if (model_draw_index == group.size()) {
        group.push_back(graphics->device().createBuffer(plume::RenderBufferDesc::UploadBuffer(
            model->vertices * 8 * sizeof(float), plume::RenderBufferFlag::VERTEX)));
    }
    auto* buffer = group[model_draw_index++].get();
    if (!buffer) return;
    // Rebuild from bind data even on invalid poses: another player's pose
    // must not become the fallback for this rider.
    AvatarPose pose = frame.pose;
    if (!model_animation_enabled || !pose.valid) { pose = {}; pose.valid = true; }
    bool posed = pose_gltf_model(*model, pose);
    if (!posed && model->rig) {
        pose = {}; pose.valid = true;
        posed = pose_gltf_model(*model, pose);
    }
    model_mirrored = posed && pose.mirrored;
    auto* destination = static_cast<float*>(buffer->map());
    if (!destination) return;
    for (const auto& primitive : model->primitives) {
        const size_t count = primitive.positions.size() / 3;
        for (size_t vertex = 0; vertex < count; ++vertex) {
            for (size_t axis = 0; axis < 3; ++axis) *destination++ = primitive.positions[vertex * 3 + axis];
            for (size_t axis = 0; axis < 3; ++axis)
                *destination++ = primitive.normals.size() == count * 3 ? primitive.normals[vertex * 3 + axis] : (axis == 1 ? 1.f : 0.f);
            for (size_t axis = 0; axis < 2; ++axis)
                *destination++ = primitive.texcoords.size() == count * 2 ? primitive.texcoords[vertex * 2 + axis] : 0.f;
        }
    }
    buffer->unmap();
    const float scale = avatar_model_scale();
    // VRM vertices are in metres, with their lowest point on the rider's
    // local origin. Use the exact Avatar world/view/projection from this frame.
    std::array<float, 28> constants{};
    const auto transform = avatar_model_to_clip(frame.world, frame.view, frame.projection,
                                                scale, model->ground_y);
    std::copy(transform.begin(), transform.end(), constants.begin());
    constants[16] = 0.4f; constants[17] = -0.7f; constants[18] = 0.6f; constants[19] = 0.45f;
    command_list->setGraphicsPipelineLayout(model_layout.get());
    const auto& viewport = raster_state->viewport();
    const bool reversed = viewport.minDepth > viewport.maxDepth;
    const auto* single_sided = reversed ? model_reverse_pipeline.get() : model_pipeline.get();
    const auto* double_sided = reversed ? model_reverse_double_sided_pipeline.get() : model_double_sided_pipeline.get();
    command_list->setPipeline(single_sided);
    const plume::RenderVertexBufferView vertex_view(plume::RenderBufferReference(buffer, 0),
                                                    uint32_t(model->vertices * 8 * sizeof(float)));
    const plume::RenderInputSlot slot(0, sizeof(float) * 8);
    command_list->setVertexBuffers(0, &vertex_view, 1, &slot);
    const plume::RenderIndexBufferView index_view(plume::RenderBufferReference(model_indices[model_mirrored ? 1 : 0].get(), 0),
                                                  model_index_count * uint32_t(sizeof(uint32_t)),
                                                  plume::RenderFormat::R32_UINT);
    command_list->setIndexBuffer(&index_view);
    command_list->setGraphicsDescriptorSet(model_sampler.get(), 1);
    for (const ModelPart& part : model_parts) {
        if (!part.count || part.picture >= model_picture_sets.size()) continue;
        command_list->setPipeline(part.double_sided ? double_sided : single_sided);
        for (int channel = 0; channel < 4; ++channel) constants[20 + size_t(channel)] = part.colour[channel];
        constants[24] = part.cutoff;
        constants[25] = part.unlit ? 1.0f : 0.0f;
        command_list->setGraphicsDescriptorSet(model_picture_sets[part.picture].get(), 0);
        command_list->setGraphicsPushConstants(0, constants.data());
        command_list->drawIndexedInstanced(part.count, 1, part.first, 0, 0);
    }
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
    uint32_t percent = 100;
    if (const char* setting = std::getenv("SFR_RENDER_SCALE"); setting && *setting) {
        const std::string value(setting);
        if (value == "50" || value == "75" || value == "100" || value == "150" || value == "200")
            percent = uint32_t(std::stoul(value));
        else std::cerr << "NATIVE_RENDER_SCALE invalid=" << value << " fallback=100\n";
    }
    implementation->render_width = (std::max)(1u, (width * percent + 50) / 100);
    implementation->render_height = (std::max)(1u, (height * percent + 50) / 100);
    if (implementation->render_width > 8192 || implementation->render_height > 8192)
        throw std::invalid_argument("scaled native presentation dimensions exceed 8192");
    std::cerr << "NATIVE_RENDER_SCALE percent=" << percent << " logical=" << width << 'x' << height
              << " physical=" << implementation->render_width << 'x' << implementation->render_height << '\n';

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
#elif defined(__APPLE__)
    // MoltenVK draws into a CAMetalLayer: SDL makes a Metal view for it.
    SDL_SysWMinfo system{};
    SDL_VERSION(&system.version);
    if (!SDL_GetWindowWMInfo(implementation->window, &system)) unavailable("native window");
    SDL_MetalView view = SDL_Metal_CreateView(implementation->window);
    if (!view) unavailable("Metal view");
    plume::RenderWindow render_window{(void*)system.info.cocoa.window, SDL_Metal_GetLayer(view)};
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
        implementation->rebuild_swap_chain_sync();
    }
    if (implementation->swap_chain->isEmpty() || implementation->swap_chain->getTextureCount() < 2)
        unavailable("swap chain");
    // Off unless asked for (Plume's Vulkan swap chain starts with it on).
    const char* vsync = std::getenv("SFR_VSYNC");
    implementation->swap_chain->setVsyncEnabled(vsync && *vsync == '1');
    if (const char* fullscreen = std::getenv("SFR_FULLSCREEN"); fullscreen && *fullscreen == '1')
        implementation->start_fullscreen = true;

    implementation->color = graphics.device().createTexture(
        plume::RenderTextureDesc::ColorTarget(implementation->render_width, implementation->render_height, plume::RenderFormat::B8G8R8A8_UNORM));
    implementation->depth = graphics.device().createTexture(
        plume::RenderTextureDesc::DepthTarget(implementation->render_width, implementation->render_height, plume::RenderFormat::D32_FLOAT_S8_UINT));
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
    if (implementation->framebuffer->getWidth() != implementation->render_width || implementation->framebuffer->getHeight() != implementation->render_height)
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
uint32_t NativePresentation::render_width() const noexcept { return impl_->render_width; }
uint32_t NativePresentation::render_height() const noexcept { return impl_->render_height; }
plume::RenderRect NativePresentation::render_rectangle(const plume::RenderRect& rectangle) const {
    return impl_->render_rectangle(rectangle);
}
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

    impl_->drain();
    std::vector<plume::RenderRect> scaled;
    if (!rectangles.empty()) {
        scaled.reserve(rectangles.size());
        for (const auto& rectangle : rectangles) {
            const auto rect = impl_->render_rectangle(rectangle);
            if (rect.left < rect.right && rect.top < rect.bottom) scaled.push_back(rect);
        }
        // A subpixel logical rectangle may vanish at a lower scale. An empty
        // rectangle array means full clear to Plume, so do not submit it.
        if (scaled.empty()) return;
        rectangles = scaled;
    }

    impl_->ensure_open();
    impl_->apply_raster();
    if (clear.color) {
        const auto& c = clear.color_value;
        impl_->command_list->clearColor(0, plume::RenderColor(c[0], c[1], c[2], c[3]), rectangles.data(), static_cast<uint32_t>(rectangles.size()));
    }
    if (clear.depth || clear.stencil)
        impl_->command_list->clearDepthStencil(clear.depth, clear.stencil, clear.depth_value, clear.stencil_value,
                                                rectangles.data(), static_cast<uint32_t>(rectangles.size()));
}

void NativePresentation::record_async_raw(RecordFunction function, const void* payload, size_t bytes) {
    impl_->refuse_during_gpu_wait();
    impl_->enqueue(function, payload, bytes);
}

void NativePresentation::record(const std::function<void(plume::RenderCommandList&)>& body) {
    impl_->refuse_during_gpu_wait();
    impl_->drain();
    impl_->ensure_open();
    impl_->apply_raster();
    body(*impl_->command_list);
}

void NativePresentation::flush() { impl_->flush(); }

uint64_t NativePresentation::list_generation() const { return impl_->list_generation; }

void NativePresentation::set_gpu_wait(std::function<void(const std::function<void()>&)> wait) {
    Impl::gpu_wait() = std::move(wait);
}

void NativePresentation::after_flush(std::function<void(bool)> callback) {
    impl_->after_flush.push_back(std::move(callback));
}

void NativePresentation::before_submit(std::function<void()> callback) {
    impl_->before_submit.push_back(std::move(callback));
}

void NativePresentation::clear_before_submit() { impl_->before_submit.clear(); }

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
        const uint64_t row_pitch = uint64_t(impl_->render_width) * bytes_per_pixel;
        auto buffer = impl_->graphics->device().createBuffer(plume::RenderBufferDesc::ReadbackBuffer(row_pitch * impl_->render_height));
        if (!buffer) unavailable("readback buffer");
        impl_->flush();
        impl_->begin_list();
        impl_->command_list->barriers(plume::RenderBarrierStage::COPY,
            plume::RenderTextureBarrier(impl_->color.get(), plume::RenderTextureLayout::COPY_SOURCE));
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {impl_->render_width, impl_->render_height, 1};
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
        std::vector<uint8_t> result(mapped, mapped + row_pitch * impl_->render_height);
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
    if (footprint.Footprint.Format != DXGI_FORMAT_B8G8R8A8_UNORM || row_count != impl_->render_height ||
        row_bytes != static_cast<UINT64>(impl_->render_width) * bytes_per_pixel ||
        footprint.Footprint.RowPitch < row_bytes || (footprint.Footprint.RowPitch & 255u) != 0)
        throw std::runtime_error("native presentation returned an invalid color copy footprint");
    auto buffer = impl_->graphics->device().createBuffer(
        plume::RenderBufferDesc::ReadbackBuffer(total_bytes));
    if (!buffer || !static_cast<plume::D3D12Buffer*>(buffer.get())->d3d) unavailable("readback buffer");

    impl_->flush();
    impl_->begin_list();
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
    std::vector<uint8_t> result(static_cast<std::size_t>(impl_->render_width) * impl_->render_height * bytes_per_pixel);
    for (uint32_t y = 0; y < impl_->render_height; ++y)
        std::memcpy(result.data() + static_cast<std::size_t>(y) * impl_->render_width * bytes_per_pixel,
                    mapped + footprint.Offset + static_cast<std::size_t>(y) * footprint.Footprint.RowPitch,
                    impl_->render_width * bytes_per_pixel);
    buffer->unmap();
    return result;
#endif
}

#ifdef __ANDROID__
bool NativePresentation::Impl::rebuild_surface() {
    flush();
    wait_presentation_queue();
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
    rebuild_swap_chain_sync();
    if (blit_pipeline) build_blit_targets();
    rebuild_swap_chain = false;
    std::cerr << "NATIVE_PRESENTATION surface rebuilt images=" << swap_chain->getTextureCount() << '\n';
    return true;
}
#endif

std::optional<std::array<float, 16>> NativePresentation::avatar_hand_transform(const AvatarPose& pose, uint32_t bone) const {
    if (!impl_->model || !impl_->model_animation_enabled) return std::nullopt;
    return gltf_avatar_bone_transform(*impl_->model, pose, bone, avatar_model_scale());
}

void NativePresentation::prepare_player_model() {
    if (sfr::model_wanted()) impl_->build_model();
}

void NativePresentation::draw_player_model(const AvatarFrameTransform& frame) {
    if (!sfr::model_wanted()) return;
    for (const auto* matrix : {&frame.world, &frame.view, &frame.projection})
        for (float value : *matrix) if (!std::isfinite(value)) return;
    impl_->refuse_during_gpu_wait();
    impl_->build_model();
    if (!impl_->model_pipeline) return;
    impl_->drain();
    impl_->ensure_open();
    impl_->apply_raster();
    impl_->draw_model(frame);
    // NativeRenderer caches layout/pipeline/descriptors/vertex bindings.
    // Force the next title draw to restore all of them after this custom pass.
    ++impl_->list_generation;
}

void NativePresentation::present(uint32_t area_width, uint32_t area_height) {
    impl_->refuse_during_gpu_wait();
    impl_->drain();

#ifdef __ANDROID__
    // Back from the background: draw into the new surface, or skip the frame.
    if ((impl_->rebuild_swap_chain || !impl_->swap_chain) && !impl_->rebuild_surface()) return;
#endif
    // The window was resized (or went full screen): the swap chain follows
    // once nothing in flight still uses its textures.
    if (impl_->swap_chain->needsResize()) {
        impl_->flush();
        impl_->wait_presentation_queue();
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
        impl_->rebuild_swap_chain_sync();
        if (impl_->blit_pipeline) impl_->build_blit_targets();
        std::cerr << "NATIVE_PRESENTATION resized images=" << impl_->swap_chain->getTextureCount()
                  << " size=" << impl_->swap_chain->getWidth() << 'x' << impl_->swap_chain->getHeight() << '\n';
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
    if (texture_index >= impl_->swap_chain->getTextureCount() ||
        (acquired && texture_index >= impl_->rendered.size()))
        throw std::runtime_error("acquired native presentation image index is out of range");
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
    // copy when the window is exactly the framebuffer and supports transfers.
    const uint32_t out_width = impl_->swap_chain->getWidth(), out_height = impl_->swap_chain->getHeight();
    const bool can_copy = impl_->graphics->backend() != GraphicsBackend::vulkan ||
        (static_cast<plume::VulkanSwapChain*>(impl_->swap_chain.get())->createInfo.imageUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    const bool stretch = !can_copy || part || out_width != impl_->render_width || out_height != impl_->render_height;
    // The copy to the swap-chain texture follows the frame's draws in their
    // own list; pipelined, the frame is submitted without waiting for it.
    const bool pipelined = Impl::pipelined();
    if (pipelined) impl_->ensure_open();
    else {
        impl_->flush();
        impl_->begin_list();
    }
    if (stretch) {
        impl_->build_blit();
        const std::array<plume::RenderTextureBarrier, 2> draw_barriers{
            plume::RenderTextureBarrier(impl_->color.get(), plume::RenderTextureLayout::SHADER_READ),
            plume::RenderTextureBarrier(swap_texture, plume::RenderTextureLayout::COLOR_WRITE)};
        impl_->command_list->barriers(plume::RenderBarrierStage::GRAPHICS, draw_barriers.data(), 2);
        impl_->command_list->setFramebuffer(impl_->blit_targets[texture_index].get());
        impl_->command_list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
        // The window keeps the framebuffer's shape, not the shown part's: a
        // console stretches the title's back buffer over the whole display,
        // so the 880x720 the loading screens set fills the same 16:9 area the
        // rest of the game does. Keeping its own shape made the loading
        // screens narrower than the game, very visibly so on a 20:9 phone.
        const float scale = (std::min)(float(out_width) / float(impl_->width),
                                       float(out_height) / float(impl_->height));
        const float view_width = float(impl_->width) * scale, view_height = float(impl_->height) * scale;
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
        const auto shown = impl_->render_rectangle({0, 0, int32_t(shown_width), int32_t(shown_height)});
        std::array<float, 32> constants{float(shown.right) / float(impl_->render_width),
                                        float(shown.bottom) / float(impl_->render_height)};
        if (touch_controls_active()) {
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

// Small built-in 5x7 glyphs keep the startup screen independent of font
// files, guest shaders, game assets, and the shader pack being prepared.
bool NativePresentation::preparation_progress(size_t completed, size_t total, bool cancelling) {
    pump_events();
#ifdef _WIN32
    if (GetForegroundWindow() == impl_->window && (GetAsyncKeyState(VK_ESCAPE) & 0x8000))
        impl_->close_requested = true;
#else
    if (SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_ESCAPE]) impl_->close_requested = true;
#endif
    cancelling = cancelling || close_requested();
    static constexpr uint8_t letters[26][5] = {
        {126,17,17,17,126},{127,73,73,73,54},{62,65,65,65,34},{127,65,65,34,28},
        {127,73,73,73,65},{127,9,9,9,1},{62,65,73,73,122},{127,8,8,8,127},
        {0,65,127,65,0},{32,64,65,63,1},{127,8,20,34,65},{127,64,64,64,64},
        {127,2,12,2,127},{127,4,8,16,127},{62,65,65,65,62},{127,9,9,9,6},
        {62,65,81,33,94},{127,9,25,41,70},{70,73,73,73,49},{1,1,127,1,1},
        {63,64,64,64,63},{31,32,64,32,31},{63,64,56,64,63},{99,20,8,20,99},
        {7,8,112,8,7},{97,81,73,69,67}};
    static constexpr uint8_t digits[10][5] = {
        {62,81,73,69,62},{0,66,127,64,0},{66,97,81,73,70},{33,65,69,75,49},
        {24,20,18,127,16},{39,69,69,69,57},{60,74,73,73,48},{1,113,9,5,3},
        {54,73,73,73,54},{6,73,73,41,30}};
    const int w = int(width()), h = int(height());
    const int scale = (std::max)(1, (std::min)(w / 200, h / 110));
    const auto rectangle = [&](int x, int y, int right, int bottom) -> plume::RenderRect {
        return {std::clamp(x,0,w), std::clamp(y,0,h), std::clamp(right,0,w), std::clamp(bottom,0,h)};
    };
    const auto text = [&](const std::string& value, int y, const std::array<float,4>& color) {
        std::vector<plume::RenderRect> pixels;
        int x = (w - int(value.size()) * 6 * scale) / 2;
        for (char c : value) {
            const uint8_t* glyph = c >= 'A' && c <= 'Z' ? letters[c-'A'] :
                c >= '0' && c <= '9' ? digits[c-'0'] : nullptr;
            if (glyph) for (int col=0; col<5; ++col) for (int row=0; row<7; ++row)
                if (glyph[col] & (1 << row)) {
                    auto rect = rectangle(x+col*scale,y+row*scale,x+(col+1)*scale,y+(row+1)*scale);
                    if (rect.left < rect.right && rect.top < rect.bottom) pixels.push_back(rect);
                }
            x += 6*scale;
        }
        // Keep clear batches small: some D3D12 drivers fail with hundreds of
        // rectangles in one ClearRenderTargetView call.
        for (size_t offset = 0; offset < pixels.size(); offset += 16)
            clear(NativeClear{.color=true,.color_value=color},
                  std::span(pixels).subspan(offset, (std::min)(size_t(16), pixels.size() - offset)));
    };
    clear(NativeClear{.color=true,.depth=true,.stencil=true,.color_value={0.018f,0.025f,0.048f,1}});
    text(cancelling ? "CANCELLING" : "PREPARING PIPELINES", h/2-20*scale,{0.90f,0.95f,1,1});
    const int left=w/6, right=w-w/6, top=h/2;
    auto background=rectangle(left,top,right,top+4*scale);
    if (background.left < background.right && background.top < background.bottom)
        clear(NativeClear{.color=true,.color_value={0.10f,0.15f,0.22f,1}},std::span(&background,1));
    const int fill = total ? int(double(right-left)*double((std::min)(completed,total))/double(total)) : 0;
    auto bar=rectangle(left,top,left+fill,top+4*scale);
    if (bar.left < bar.right && bar.top < bar.bottom)
        clear(NativeClear{.color=true,.color_value={0.10f,0.72f,0.95f,1}},std::span(&bar,1));
    text(std::to_string(completed)+" OF "+std::to_string(total),h/2+10*scale,{0.60f,0.78f,0.9f,1});
#ifdef __ANDROID__
    text("HOLD BACK TO CANCEL",h/2+25*scale,{0.45f,0.55f,0.65f,1});
#else
    text("ESC TO CANCEL",h/2+25*scale,{0.45f,0.55f,0.65f,1});
#endif
    present();
    return !close_requested();
}

void NativePresentation::finish_preparation() {
    flush();
    clear(NativeClear{.color=true,.depth=true,.stencil=true,.color_value={0,0,0,1}});
    flush();
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
    if (touch_controls_active()) impl_->read_touch_controls();
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
