#include "native_graphics.h"
#include "native_pipeline_cache.h"
#include "plume_vulkan.h"

#ifdef _WIN32
#include "plume_d3d12.h"
#endif
#include "plume_render_interface.h"
#include "spirv_bounded_descriptors.h"
#include "spirv_clip_distance.h"
#include "spirv_version.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <string>
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
#elif defined(__ANDROID__) || defined(__APPLE__)
// Swap chains take an ANativeWindow (Android) or a Cocoa window and its
// CAMetalLayer (macOS, through MoltenVK).
std::unique_ptr<RenderInterface> CreateVulkanInterface();
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

std::string hex(HRESULT value) {
    std::ostringstream text;
    text << "0x" << std::hex << static_cast<uint32_t>(value);
    return text.str();
}

// Whether Plume can draw with D3D12 here, by its own tests: the first
// hardware adapter that makes an ID3D12Device8 at feature level 11_0,
// answers the shader model query and records into the command list Plume
// uses. Each adapter's answer is logged, so a machine that fails says which
// adapter failed and how.
bool d3d12_usable() {
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    if (const HRESULT result = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)); FAILED(result)) {
        std::cerr << "NATIVE_GRAPHICS_D3D12_PROBE factory=" << hex(result) << '\n';
        return false;
    }
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
        DXGI_ADAPTER_DESC1 description{};
        adapter->GetDesc1(&description);
        char name[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, name, sizeof name - 1, nullptr, nullptr);
        std::cerr << "NATIVE_GRAPHICS_D3D12_PROBE adapter=" << index << " name=\"" << name << '"';
        if (description.Flags & (DXGI_ADAPTER_FLAG_REMOTE | DXGI_ADAPTER_FLAG_SOFTWARE)) {
            std::cerr << " skipped=remote-or-software\n";
            continue;
        }
        Microsoft::WRL::ComPtr<ID3D12Device8> device;
        if (const HRESULT result = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
            FAILED(result)) {
            std::cerr << " create=" << hex(result) << '\n';
            continue;
        }
        D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_0};
        const HRESULT result = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof model);
        if (FAILED(result) && result != E_INVALIDARG) {
            std::cerr << " shader_model=" << hex(result) << '\n';
            continue;
        }
        // Plume records into ID3D12GraphicsCommandList7, which only a recent
        // runtime has (Windows 11 24H2's own, or the Agility SDK's beside
        // the program); Windows 10's own makes the device but not the list.
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList7> list;
        if (const HRESULT made = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
            FAILED(made)) {
            std::cerr << " command_allocator=" << hex(made) << '\n';
            continue;
        }
        if (const HRESULT made = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                           IID_PPV_ARGS(&list));
            FAILED(made)) {
            std::cerr << " command_list7=" << hex(made) << " (no D3D12 Agility SDK runtime in D3D12\\?)\n";
            continue;
        }
        std::cerr << " usable=1\n";
        return true;
    }
    return false;
}
#endif
}

GraphicsBackend selected_graphics_backend() {
    static const GraphicsBackend backend = [] {
#ifdef _WIN32
        const char* text = std::getenv("SFR_GRAPHICS");
        if (text && std::strcmp(text, "vulkan") == 0) return GraphicsBackend::vulkan;
        // Chosen before anything is drawn, so the shader cache prepares
        // Vulkan's bytecode from the start when D3D12 cannot run.
        if (!d3d12_usable()) {
            std::cerr << "NATIVE_GRAPHICS_FALLBACK from=D3D12 to=Vulkan reason=no-usable-d3d12-adapter\n";
            return GraphicsBackend::vulkan;
        }
        return GraphicsBackend::d3d12;
#else
        return GraphicsBackend::vulkan;
#endif
    }();
    return backend;
}

const char* graphics_backend_name(GraphicsBackend backend) {
    return backend == GraphicsBackend::vulkan ? "Vulkan" : "D3D12";
}

std::string graphics_driver_version(GraphicsBackend backend, uint32_t vendor_id, uint64_t packed) {
    if (!packed) return "unknown";
    std::ostringstream text;
    if (backend == GraphicsBackend::d3d12)
        text << ((packed >> 48) & 0xFFFF) << '.' << ((packed >> 32) & 0xFFFF) << '.' << ((packed >> 16) & 0xFFFF)
             << '.' << (packed & 0xFFFF);
    else if (vendor_id == 0x10DE)
        text << ((packed >> 22) & 0x3FF) << '.' << ((packed >> 14) & 0xFF) << '.' << ((packed >> 6) & 0xFF);
    else
        text << ((packed >> 22) & 0x3FF) << '.' << ((packed >> 12) & 0x3FF) << '.' << (packed & 0xFFF);
    return text.str();
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
        pipeline_cache.reset();
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
    std::unique_ptr<NativePipelineCache> pipeline_cache;
    bool bounded_descriptors = false;
    BoundedLengths bounded_lengths;
    plume::RenderFormat depth_format = plume::RenderFormat::D32_FLOAT_S8_UINT;
    bool without_clip_distance = false;
    bool spirv_1_3 = false;
};

NativeGraphics::NativeGraphics() = default;
NativeGraphics::~NativeGraphics() = default;

void NativeGraphics::initialize() {
    if (impl_) return;
    const GraphicsBackend backend = selected_graphics_backend();
    const char* name = graphics_backend_name(backend);
    std::cerr << "NATIVE_GRAPHICS_INITIALIZE backend=" << name << '\n';

    // SFR_TEST_VULKAN_1_1=1 makes the Vulkan instance ask for 1.1, so that
    // the validation layer holds the device to that version's rules (the
    // shaders as SPIR-V 1.3, no 1.2 features without their extensions), as
    // on a Vulkan 1.1 driver such as a Mali-G76's (Issue #169).
    const auto test_switch = [&](const char* variable) {
        const char* value = std::getenv(variable);
        return backend == GraphicsBackend::vulkan && value && std::strcmp(value, "1") == 0;
    };
    plume::VulkanInterface::testApiVersion = test_switch("SFR_TEST_VULKAN_1_1") ? VK_API_VERSION_1_1 : 0;
#ifdef _WIN32
    auto render_interface = backend == GraphicsBackend::vulkan ? plume::CreateVulkanInterface() : plume::CreateD3D12Interface();
#else
    // SDL2 gives the Vulkan instance extensions for a window only.
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
        throw std::runtime_error(std::string("failed to start SDL video: ") + SDL_GetError());
    SDL_Window* window = SDL_CreateWindow("Sonic Free Riders", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
#ifdef __APPLE__
                                          SDL_WINDOW_METAL | SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
#else
                                          SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
#endif
    if (!window) throw std::runtime_error(std::string("failed to create the SDL window: ") + SDL_GetError());
#if defined(__ANDROID__) || defined(__APPLE__)
    auto render_interface = plume::CreateVulkanInterface();
#else
    auto render_interface = plume::CreateVulkanInterface(window);
#endif
    if (!render_interface) SDL_DestroyWindow(window);
#endif
    plume::VulkanInterface::testApiVersion = 0;
    if (!render_interface)
        throw std::runtime_error(std::string("failed to create the ") + name + " render interface");

    // SFR_TEST_NO_SCALAR_LAYOUT=1 and SFR_TEST_NO_DEVICE_ADDRESS=1 make the
    // device without scalar block layout and buffer device addresses, which
    // a Vulkan 1.1 Mali-G76 lacks (Issue #169); nothing the renderer does
    // needs them.
    const bool test_no_indexing = test_switch("SFR_TEST_NO_DESCRIPTOR_INDEXING");
    plume::VulkanDevice::disableDescriptorIndexing = test_no_indexing;
    plume::VulkanDevice::disableScalarBlockLayout = test_switch("SFR_TEST_NO_SCALAR_LAYOUT");
    plume::VulkanDevice::disableBufferDeviceAddress = test_switch("SFR_TEST_NO_DEVICE_ADDRESS");
    auto render_device = render_interface->createDevice();
    plume::VulkanDevice::disableDescriptorIndexing = false;
    plume::VulkanDevice::disableScalarBlockLayout = false;
    plume::VulkanDevice::disableBufferDeviceAddress = false;
    if (!render_device)
        throw std::runtime_error(std::string("failed to create a ") + name + " render device");
    // Which adapter the game runs on, for a performance report (scripts/
    // benchmark_summary.py reads this line): a laptop with two GPUs, or an
    // external one, can land on the wrong one. The type is Vulkan's; D3D12
    // does not say. The memory is the adapter's own, so an integrated GPU's
    // is small, and it shares the system's.
    {
        const auto& description = render_device->getDescription();
        const char* type = description.type == plume::RenderDeviceType::INTEGRATED ? "integrated" :
                           description.type == plume::RenderDeviceType::DISCRETE ? "discrete" :
                           description.type == plume::RenderDeviceType::VIRTUAL ? "virtual" :
                           description.type == plume::RenderDeviceType::CPU ? "cpu" : "unknown";
        std::cerr << "NATIVE_GRAPHICS backend=" << name << " adapter=" << description.name
                  << " vendor=0x" << std::hex << uint32_t(description.vendor) << std::dec << " type=" << type
                  << " vram_mb=" << description.dedicatedVideoMemory / (1024 * 1024)
                  << " driver=" << graphics_driver_version(backend, uint32_t(description.vendor), description.driverVersion)
                  << '\n';
    }

    // The shaders are SPIR-V 1.5, which needs Vulkan 1.2. A Vulkan 1.1 device
    // (the Mali-G76 of Issue #169) takes them as SPIR-V 1.3 (spirv_version.h).
    // The version is the lower of the instance's and the device's.
    bool spirv_1_3 = false;
    if (backend == GraphicsBackend::vulkan) {
        const auto* vulkan_device = static_cast<plume::VulkanDevice*>(render_device.get());
        const uint32_t version = (std::min)(static_cast<plume::VulkanInterface*>(render_interface.get())->appInfo.apiVersion,
                                          vulkan_device->physicalDeviceProperties.apiVersion);
        spirv_1_3 = version < VK_API_VERSION_1_2;
        const auto& capabilities = render_device->getCapabilities();
        std::cerr << "NATIVE_SPIRV api=" << VK_API_VERSION_MAJOR(version) << '.' << VK_API_VERSION_MINOR(version)
                  << " version=" << (spirv_1_3 ? "1.3" : "1.5") << " scalar_block_layout="
                  << capabilities.scalarBlockLayout << " device_address=" << capabilities.bufferDeviceAddress << '\n';
    }

    // The renderer binds every texture through one array of 4096 that is
    // only partly filled (native_renderer.cpp), which Vulkan allows only with
    // descriptor indexing. A driver without it (Mesa's hasvk on a Broadwell
    // HD 5500, issue #134: 128 textures a stage) crashed while compiling the
    // first pipeline. There each draw binds small sets of its own instead,
    // which need only Vulkan 1.0's dynamic indexing and a few dozen textures
    // a stage; a driver short even of that stops with the reason. Old Mali
    // drivers (a Mali-G72's: 16 textures a stage) get its compact form.
    // SFR_TEST_BOUNDED_TEXTURES=<n> takes n as the textures a stage.
    bool bounded_descriptors = false;
    BoundedLengths bounded_lengths;
    if (backend == GraphicsBackend::vulkan && !render_device->getCapabilities().descriptorIndexing) {
        const VkPhysicalDevice physical = static_cast<plume::VulkanDevice*>(render_device.get())->physicalDevice;
        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeatures(physical, &features);
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical, &properties);
        auto& limits = properties.limits;
        if (const char* forced = std::getenv("SFR_TEST_BOUNDED_TEXTURES"); forced && std::atoi(forced) > 0)
            limits.maxPerStageDescriptorSampledImages = uint32_t(std::atoi(forced));
        const auto lengths = sfr::bounded_lengths(
            (std::min)(limits.maxPerStageDescriptorSampledImages, limits.maxDescriptorSetSampledImages));
        if (lengths) bounded_lengths = *lengths;
        const uint32_t textures = bounded_lengths.textures(), samplers = bounded_descriptor_count;
        // Set 2: vertex, pixel and shared constants, palette, loop constants.
        constexpr uint32_t uniform_buffers = 5;
        std::string missing;
        const auto need = [&](bool met, const std::string& what) {
            if (!met) missing += (missing.empty() ? "" : ", ") + what;
        };
        need(features.shaderSampledImageArrayDynamicIndexing, "shaderSampledImageArrayDynamicIndexing");
        need(limits.maxBoundDescriptorSets >= 3, "3 descriptor sets (has " + std::to_string(limits.maxBoundDescriptorSets) + ")");
        need(limits.maxPerStageDescriptorSampledImages >= textures, std::to_string(textures) + " textures a stage (has " +
             std::to_string(limits.maxPerStageDescriptorSampledImages) + ")");
        need(limits.maxDescriptorSetSampledImages >= textures, std::to_string(textures) + " textures a layout (has " +
             std::to_string(limits.maxDescriptorSetSampledImages) + ")");
        need(limits.maxPerStageDescriptorSamplers >= samplers && limits.maxDescriptorSetSamplers >= samplers,
             std::to_string(samplers) + " samplers");
        need(limits.maxDescriptorSetUniformBuffersDynamic >= uniform_buffers &&
             limits.maxPerStageDescriptorUniformBuffers >= uniform_buffers,
             std::to_string(uniform_buffers) + " dynamic uniform buffers");
        need(limits.maxPerStageResources >= textures + samplers + uniform_buffers + 1,
             std::to_string(textures + samplers + uniform_buffers + 1) + " resources a stage (has " +
             std::to_string(limits.maxPerStageResources) + ")");
        if (!missing.empty())
            throw std::runtime_error("this GPU's Vulkan driver lacks descriptor indexing (runtime descriptor arrays, "
                                     "partially bound and variable-count bindings), and also what the renderer's "
                                     "fallback for that needs (" + missing + "); the GPU is not supported");
        bounded_descriptors = true;
        std::cerr << "NATIVE_DESCRIPTORS mode=bounded forced=" << test_no_indexing << " textures_per_stage="
                  << limits.maxPerStageDescriptorSampledImages << " samplers_per_stage="
                  << limits.maxPerStageDescriptorSamplers << " resources_per_stage=" << limits.maxPerStageResources
                  << " compact=" << bounded_lengths.compact() << '\n';
    }

    // Every vertex shader writes a clip distance (spirv_clip_distance.h).
    // Mali drivers crash compiling that write (Issues #166, #169); a device
    // without shaderClipDistance may not take it at all. Either way the
    // shaders go without it. SFR_TEST_NO_CLIP_DISTANCE=1 does the same on any
    // Vulkan device, to try it, and =0 keeps it, to see the crash.
    bool without_clip_distance = false;
    if (backend == GraphicsBackend::vulkan) {
        const VkPhysicalDevice physical = static_cast<plume::VulkanDevice*>(render_device.get())->physicalDevice;
        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeatures(physical, &features);
        const char* no_clip = std::getenv("SFR_TEST_NO_CLIP_DISTANCE");
        const bool forced = no_clip && (std::strcmp(no_clip, "1") == 0 || std::strcmp(no_clip, "0") == 0);
        without_clip_distance = forced ? no_clip[0] == '1' : !features.shaderClipDistance;
        std::cerr << "NATIVE_CLIP_DISTANCE supported=" << features.shaderClipDistance << " forced=" << forced
                  << " removed=" << without_clip_distance << '\n';
    }

    // Depth targets are D32_SFLOAT_S8_UINT, drawn into and sampled (resolved
    // depth). An old Mali driver (a Mali-G72's, r19) has only
    // D24_UNORM_S8_UINT; Plume made a texture object without an image there,
    // and the driver crashed making the presentation's framebuffer view.
    // Such a driver gets D24S8; one with neither stops with the reason.
    plume::RenderFormat depth_format = plume::RenderFormat::D32_FLOAT_S8_UINT;
    if (backend == GraphicsBackend::vulkan) {
        const VkPhysicalDevice physical = static_cast<plume::VulkanDevice*>(render_device.get())->physicalDevice;
        const auto usable = [&](VkFormat format) {
            constexpr VkFormatFeatureFlags needed =
                VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physical, format, &properties);
            return (properties.optimalTilingFeatures & needed) == needed;
        };
        const bool d32s8 = usable(VK_FORMAT_D32_SFLOAT_S8_UINT), d24s8 = usable(VK_FORMAT_D24_UNORM_S8_UINT);
        const bool forced = test_switch("SFR_TEST_DEPTH_D24S8");
        if ((forced || !d32s8) && d24s8) depth_format = plume::RenderFormat::D24_UNORM_S8_UINT;
        else if (!d32s8)
            throw std::runtime_error("this GPU's Vulkan driver has neither the D32_SFLOAT_S8_UINT nor the "
                                     "D24_UNORM_S8_UINT depth format the renderer needs; the GPU is not supported");
        std::cerr << "NATIVE_DEPTH format=" << (depth_format == plume::RenderFormat::D32_FLOAT_S8_UINT ? "D32S8" : "D24S8")
                  << " d32s8=" << d32s8 << " d24s8=" << d24s8 << " forced=" << forced << '\n';
    }

    auto render_queue = render_device->createCommandQueue(plume::RenderCommandListType::DIRECT);
    if (!render_queue)
        throw std::runtime_error(std::string("failed to create a ") + name + " direct command queue");

    auto implementation = std::make_unique<Impl>();
    implementation->backend = backend;
    implementation->bounded_descriptors = bounded_descriptors;
    implementation->bounded_lengths = bounded_lengths;
    implementation->depth_format = depth_format;
    implementation->without_clip_distance = without_clip_distance;
    implementation->spirv_1_3 = spirv_1_3;
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
    const char* cache = std::getenv("SFR_PIPELINE_CACHE");
    if (backend == GraphicsBackend::vulkan && (!cache || std::strcmp(cache, "0") != 0)) {
        try {
            implementation->pipeline_cache = std::make_unique<NativePipelineCache>(
                *static_cast<plume::VulkanDevice*>(implementation->device.get()));
        } catch (const std::exception& e) {
            std::cerr << std::string("NATIVE_PIPELINE_CACHE initialization_failed=") + e.what() + '\n';
        }
    }
    impl_ = std::move(implementation);
}

bool NativeGraphics::bounded_descriptors() const noexcept {
    return impl_ && impl_->bounded_descriptors;
}

BoundedLengths NativeGraphics::bounded_lengths() const noexcept {
    return impl_ ? impl_->bounded_lengths : BoundedLengths{};
}

plume::RenderFormat NativeGraphics::depth_format() const noexcept {
    return impl_ ? impl_->depth_format : plume::RenderFormat::D32_FLOAT_S8_UINT;
}

bool NativeGraphics::rewrites_shaders() const noexcept {
    return impl_ && (impl_->bounded_descriptors || impl_->without_clip_distance || impl_->spirv_1_3);
}

std::vector<uint8_t> NativeGraphics::shader_code(std::span<const uint8_t> code) const {
    std::vector<uint8_t> result(code.begin(), code.end());
    std::string why;
    if (impl_ && impl_->without_clip_distance) {
        auto rewritten = without_clip_distance_spirv(result, &why);
        if (!rewritten) throw std::runtime_error("a shader cannot go without its clip distance: " + why);
        result = std::move(*rewritten);
    }
    if (bounded_descriptors()) {
        auto rewritten = bounded_descriptor_spirv(result, &why, impl_->bounded_lengths);
        if (!rewritten) throw std::runtime_error("a shader cannot use the bounded descriptor layout: " + why);
        result = std::move(*rewritten);
    }
    if (impl_ && impl_->spirv_1_3) {
        auto rewritten = spirv_1_3(result, &why);
        if (!rewritten) throw std::runtime_error("a shader cannot be SPIR-V 1.3: " + why);
        result = std::move(*rewritten);
    }
    return result;
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
