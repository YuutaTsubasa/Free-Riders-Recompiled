#include "native_graphics.h"

#ifdef _WIN32
#include "plume_d3d12.h"
#endif
#include "plume_render_interface.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <sstream>
#include <thread>
#include <utility>
#ifdef _WIN32
#include <wrl/client.h>
#else
#include <SDL.h>
#endif

namespace plume {
#if defined(_WIN32)
std::unique_ptr<RenderInterface> CreateD3D12Interface();
std::unique_ptr<RenderInterface> CreateVulkanInterface();
#elif defined(__ANDROID__)
std::unique_ptr<RenderInterface> CreateVulkanInterface();  // swap chains take an ANativeWindow
#else
std::unique_ptr<RenderInterface> CreateVulkanInterface(RenderWindow sdlWindow);
#endif
}

namespace sfr {
namespace {
#ifdef _WIN32
[[noreturn]] void d3d_failure(const char* operation, HRESULT result, ID3D12Device* device) {
    const HRESULT removed = device ? device->GetDeviceRemovedReason() : S_OK;
    std::ostringstream message;
    message << operation << " failed with HRESULT 0x" << std::hex << static_cast<uint32_t>(result);
    if (FAILED(removed))
        message << "; device removal HRESULT 0x" << static_cast<uint32_t>(removed);
    throw std::runtime_error(message.str());
}
#endif
}

GraphicsBackend selected_graphics_backend() {
    static const GraphicsBackend backend = [] {
#ifdef _WIN32
        const char* text = std::getenv("SFR_GRAPHICS");
        return text && std::strcmp(text, "vulkan") == 0 ? GraphicsBackend::vulkan : GraphicsBackend::d3d12;
#else
        return GraphicsBackend::vulkan;
#endif
    }();
    return backend;
}

const char* graphics_backend_name(GraphicsBackend backend) {
    return backend == GraphicsBackend::vulkan ? "Vulkan" : "D3D12";
}

struct NativeGraphics::Impl {
    GraphicsBackend backend = GraphicsBackend::d3d12;
    std::unique_ptr<plume::RenderInterface> render_interface;
    std::unique_ptr<plume::RenderDevice> device;
    std::unique_ptr<plume::RenderCommandQueue> queue;
    // D3D12: a native fence polled with a timeout. Vulkan: an empty
    // submission signalling a Plume fence.
#ifdef _WIN32
    Microsoft::WRL::ComPtr<ID3D12Fence> idle_fence;
    UINT64 idle_value = 0;
#else
    SDL_Window* window = nullptr;
    ~Impl() {
        idle_signal.reset();
        idle_list.reset();
        queue.reset();
        device.reset();
        render_interface.reset();
        if (window) SDL_DestroyWindow(window);
    }
#endif
    std::unique_ptr<plume::RenderCommandList> idle_list;
    std::unique_ptr<plume::RenderCommandFence> idle_signal;
};

NativeGraphics::NativeGraphics() = default;
NativeGraphics::~NativeGraphics() = default;

void NativeGraphics::initialize() {
    if (impl_) return;
    const GraphicsBackend backend = selected_graphics_backend();
    const char* name = graphics_backend_name(backend);

#ifdef _WIN32
    auto render_interface = backend == GraphicsBackend::vulkan ? plume::CreateVulkanInterface() : plume::CreateD3D12Interface();
#else
    // SDL2 gives the Vulkan instance extensions for a window only.
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
        throw std::runtime_error(std::string("failed to start SDL video: ") + SDL_GetError());
    SDL_Window* window = SDL_CreateWindow("Sonic Free Riders", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
                                          SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
    if (!window) throw std::runtime_error(std::string("failed to create the SDL window: ") + SDL_GetError());
#ifdef __ANDROID__
    auto render_interface = plume::CreateVulkanInterface();
#else
    auto render_interface = plume::CreateVulkanInterface(window);
#endif
    if (!render_interface) SDL_DestroyWindow(window);
#endif
    if (!render_interface)
        throw std::runtime_error(std::string("failed to create the ") + name + " render interface");

    auto render_device = render_interface->createDevice();
    if (!render_device)
        throw std::runtime_error(std::string("failed to create a ") + name + " render device");

    auto render_queue = render_device->createCommandQueue(plume::RenderCommandListType::DIRECT);
    if (!render_queue)
        throw std::runtime_error(std::string("failed to create a ") + name + " direct command queue");

    auto implementation = std::make_unique<Impl>();
    implementation->backend = backend;
#ifndef _WIN32
    implementation->window = window;
#endif
#ifdef _WIN32
    if (backend == GraphicsBackend::d3d12) {
        const auto* const d3d12_queue = static_cast<const plume::D3D12CommandQueue*>(render_queue.get());
        if (!d3d12_queue->d3d)
            throw std::runtime_error("failed to create the native D3D12 direct command queue");
        const auto* const d3d12_device = static_cast<const plume::D3D12Device*>(render_device.get());
        if (!d3d12_device->d3d)
            throw std::runtime_error("failed to create the native D3D12 render device");
        const HRESULT fence_result = d3d12_device->d3d->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&implementation->idle_fence));
        if (FAILED(fence_result))
            d3d_failure("D3D12 idle fence creation", fence_result, d3d12_device->d3d);
    } else
#endif
    {
        implementation->idle_list = render_queue->createCommandList();
        implementation->idle_signal = render_device->createCommandFence();
        if (!implementation->idle_list || !implementation->idle_signal)
            throw std::runtime_error("failed to create the Vulkan idle synchronization objects");
    }
    implementation->render_interface = std::move(render_interface);
    implementation->device = std::move(render_device);
    implementation->queue = std::move(render_queue);
    impl_ = std::move(implementation);
}

bool NativeGraphics::initialized() const noexcept {
    return impl_ != nullptr;
}

void* NativeGraphics::host_window() const noexcept {
#ifdef _WIN32
    return nullptr;
#else
    return impl_ ? impl_->window : nullptr;
#endif
}

GraphicsBackend NativeGraphics::backend() const noexcept {
    return impl_ ? impl_->backend : selected_graphics_backend();
}

plume::RenderDevice& NativeGraphics::device() {
    if (!impl_) throw std::logic_error("native graphics is not initialized");
    if (!impl_->device) throw std::runtime_error("native graphics device is unavailable");
    return *impl_->device;
}

plume::RenderCommandQueue& NativeGraphics::queue() {
    if (!impl_) throw std::logic_error("native graphics is not initialized");
    if (!impl_->queue) throw std::runtime_error("native graphics queue is unavailable");
    return *impl_->queue;
}

void NativeGraphics::wait_idle(const std::function<bool()>& cancelled) {
    if (!impl_) throw std::logic_error("native graphics is not initialized");
    if (!impl_->device || !impl_->queue)
        throw std::runtime_error("native graphics synchronization objects are unavailable");
    if (cancelled && cancelled())
        throw std::runtime_error("native graphics idle wait was cancelled before queue synchronization");

    if (impl_->backend == GraphicsBackend::vulkan) {
        // Work is submitted in order: an empty list completing means
        // everything before it has.
        impl_->idle_list->begin();
        impl_->idle_list->end();
        const plume::RenderCommandList* lists[] = {impl_->idle_list.get()};
        impl_->queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, impl_->idle_signal.get());
        if (cancelled && cancelled()) {
            impl_->queue->waitForCommandFence(impl_->idle_signal.get());  // the list must not be reused in flight
            throw std::runtime_error("native graphics idle wait was cancelled");
        }
        impl_->queue->waitForCommandFence(impl_->idle_signal.get());
        return;
    }
#ifdef _WIN32

    if (!impl_->idle_fence)
        throw std::runtime_error("native graphics synchronization objects are unavailable");
    auto* const device = static_cast<plume::D3D12Device*>(impl_->device.get());
    auto* const queue = static_cast<plume::D3D12CommandQueue*>(impl_->queue.get());
    if (!device->d3d || !queue->d3d)
        throw std::runtime_error("native D3D12 synchronization objects are unavailable");
    if (impl_->idle_value >= UINT64_MAX - 1)
        throw std::runtime_error("native graphics idle fence value exhausted");

    const UINT64 completion_value = impl_->idle_value + 1;
    const HRESULT result = queue->d3d->Signal(impl_->idle_fence.Get(), completion_value);
    if (FAILED(result)) d3d_failure("D3D12 queue idle signal", result, device->d3d);
    impl_->idle_value = completion_value;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        if (cancelled && cancelled())
            throw std::runtime_error("native graphics idle wait was cancelled");
        const UINT64 completed = impl_->idle_fence->GetCompletedValue();
        if (completed == UINT64_MAX)
            d3d_failure("D3D12 queue idle completion", DXGI_ERROR_DEVICE_REMOVED, device->d3d);
        if (completed >= completion_value) return;
        if (std::chrono::steady_clock::now() >= deadline) {
            const HRESULT removed = device->d3d->GetDeviceRemovedReason();
            if (FAILED(removed)) d3d_failure("D3D12 queue idle completion", removed, device->d3d);
            throw std::runtime_error("D3D12 queue idle wait timed out after 5 seconds");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
#endif
}
}
