#include "native_graphics.h"
#include "native_presentation.h"
#include "native_raster_state.h"
#include "avatar_transform.h"
#include "gltf_model.h"

#include "plume_render_interface.h"
#include <volk.h>

#ifdef _WIN32
#include "plume_d3d12.h"
#define NOMINMAX
#include <windows.h>
#endif

#include <array>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
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

struct ScopedEnvironment {
    std::string name;
    std::optional<std::string> previous;
    static void set(const std::string& name, const char* value) {
#ifdef _WIN32
        _putenv_s(name.c_str(), value ? value : "");
#else
        if (value) setenv(name.c_str(), value, 1);
        else unsetenv(name.c_str());
#endif
    }
    ScopedEnvironment(std::string key, const std::string& value) : name(std::move(key)) {
        if (const char* old = std::getenv(name.c_str())) previous = old;
        set(name, value.c_str());
    }
    ~ScopedEnvironment() { set(name, previous ? previous->c_str() : nullptr); }
};

// A real model upload with no external assets. Root translation changes the
// generated vertex data, so two draws detect accidental upload-buffer reuse.
struct AvatarTriangleFile {
    std::filesystem::path directory, path;
    AvatarTriangleFile() {
        directory = std::filesystem::temp_directory_path() /
            ("sfr-avatar-presentation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(std::filesystem::create_directory(directory), "create isolated Avatar fixture directory");
        path = directory / "triangle.glb";
        const auto put32 = [](std::vector<uint8_t>& out, uint32_t value) {
            for (unsigned shift = 0; shift < 32; shift += 8) out.push_back(uint8_t(value >> shift));
        };
        std::vector<uint8_t> binary;
        for (float value : {-0.25f, 0.f, 0.f, 0.25f, 0.f, 0.f, 0.f, 0.75f, 0.f}) {
            uint32_t bits;
            std::memcpy(&bits, &value, sizeof(bits));
            put32(binary, bits);
        }
        // JOINTS_0: every vertex uses joint zero. WEIGHTS_0 assigns it full
        // influence; an actual skin is required for native pose evaluation.
        binary.insert(binary.end(), 12, 0);
        for (float value : {1.f,0.f,0.f,0.f, 1.f,0.f,0.f,0.f, 1.f,0.f,0.f,0.f,
                            1.f,0.f,0.f,0.f, 0.f,1.f,0.f,0.f, 0.f,0.f,1.f,0.f, 0.f,0.f,0.f,1.f}) {
            uint32_t bits;
            std::memcpy(&bits, &value, sizeof(bits));
            put32(binary, bits);
        }
        for (uint8_t value : {0, 0, 1, 0, 2, 0, 0, 0}) binary.push_back(value);
        std::string json = R"({"asset":{"version":"2.0"},
            "nodes":[{"mesh":0,"skin":0},{}],
            "skins":[{"joints":[1],"inverseBindMatrices":4}],
            "meshes":[{"primitives":[{"attributes":{"POSITION":0,"JOINTS_0":2,"WEIGHTS_0":3},"indices":1,"material":0}]}],
            "materials":[{"doubleSided":true,"pbrMetallicRoughness":{"baseColorFactor":[1,0,0,1]},
                "extensions":{"KHR_materials_unlit":{}}}],
            "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
                {"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"},
                {"bufferView":2,"componentType":5121,"count":3,"type":"VEC4"},
                {"bufferView":3,"componentType":5126,"count":3,"type":"VEC4"},
                {"bufferView":4,"componentType":5126,"count":1,"type":"MAT4"}],
            "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},
                {"buffer":0,"byteOffset":160,"byteLength":6},
                {"buffer":0,"byteOffset":36,"byteLength":12},
                {"buffer":0,"byteOffset":48,"byteLength":48},
                {"buffer":0,"byteOffset":96,"byteLength":64}],
            "buffers":[{"byteLength":168}],
            "extensions":{"VRMC_vrm":{"humanoid":{"humanBones":{"hips":{"node":1}}}}}})";
        while (json.size() % 4) json += ' ';
        std::vector<uint8_t> glb;
        put32(glb, 0x46546C67); put32(glb, 2); put32(glb, uint32_t(28 + json.size() + binary.size()));
        put32(glb, uint32_t(json.size())); put32(glb, 0x4E4F534A);
        glb.insert(glb.end(), json.begin(), json.end());
        put32(glb, uint32_t(binary.size())); put32(glb, 0x004E4942);
        glb.insert(glb.end(), binary.begin(), binary.end());
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(glb.data()), std::streamsize(glb.size()));
        require(bool(file), "write the synthetic Avatar GLB");
    }
    ~AvatarTriangleFile() {
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::remove(directory, error);
    }
};

void avatar_draws_keep_viewport_order_and_independent_poses() {
    AvatarTriangleFile fixture;
    ScopedEnvironment model("SFR_AVATAR_MODEL", fixture.path.string());
    ScopedEnvironment scale("SFR_AVATAR_MODEL_SCALE", "1");
    ScopedEnvironment pose("SFR_AVATAR_MODEL_POSE", "game");
    ScopedEnvironment depth("SFR_AVATAR_MODEL_NO_DEPTH", "0");
    std::string model_error;
    auto fixture_model = sfr::load_binary_gltf(fixture.path, &model_error, sfr::GltfPose::rest);
    require(fixture_model && fixture_model->rig, "synthetic Avatar has a real native-pose rig");
    sfr::AvatarPose translated_pose;
    translated_pose.valid = true;
    translated_pose.bones[0].translation[0] = -0.55f;
    require(sfr::pose_gltf_model(*fixture_model, translated_pose) &&
                std::abs(fixture_model->primitives[0].positions[0] - (-0.8f)) < 0.0001f,
            "fixture root translation changes uploaded vertex positions");
    sfr::NativeGraphics graphics;
    graphics.initialize();
    sfr::NativePresentation presentation(graphics, 64, 48);
    sfr::NativeClear clear{};
    clear.color = true;
    clear.depth = true;
    clear.depth_value = 1;
    clear.color_value = {0, 0, 0, 1};
    presentation.clear(clear);
    const plume::RenderViewport viewport(8, 8, 48, 32, 0, 1);
    const plume::RenderRect scissor(16, 12, 48, 36);
    presentation.set_raster_state(viewport, scissor);
    const sfr::AvatarMatrix identity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    sfr::AvatarFrameTransform frame{identity, identity, identity};
    frame.world[13] = -0.5f;
    frame.world[14] = 0.5f;
    frame.pose.valid = true;
    frame.pose.bones[0].translation[0] = -0.55f;
    frame.present = 7;
    const auto before = presentation.list_generation();
    presentation.draw_player_model(frame);
    const auto after_first = presentation.list_generation();
    require(after_first > before, "Avatar pass invalidates cached guest bindings");
    // No flush/readback between these draws: the GPU must retain both poses.
    frame.pose.bones[0].translation[0] = 0.55f;
    presentation.draw_player_model(frame);
    require(presentation.list_generation() > after_first, "each Avatar pass invalidates guest bindings");
    require(presentation.raster_state().viewport() == viewport && presentation.raster_state().scissor() == scissor,
            "Avatar pass retains the title's viewport and scissor");
    clear.color_value = {0, 1, 0, 1};
    clear.depth = false;
    const plume::RenderRect overlay(18, 28, 21, 31);
    presentation.clear(clear, std::span<const plume::RenderRect>(&overlay, 1));
    presentation.present();
    const auto pixels = presentation.readback_color();
    require(pixels.size() == 64 * 48 * 4, "Avatar GPU readback contains the complete target");
    const auto pixel = [&](uint32_t x, uint32_t y) {
        std::array<uint8_t, 4> value;
        std::memcpy(value.data(), pixels.data() + (y * 64 + x) * 4, 4);
        return value;
    };
    require(pixel(19, 25) == std::array<uint8_t, 4>{0, 0, 255, 255},
            "first draw keeps its own root-translated vertices until submission");
    require(pixel(45, 29) == std::array<uint8_t, 4>{0, 0, 255, 255},
            "second draw uses its own root-translated vertices in the partial viewport");
    require(pixel(19, 29) == std::array<uint8_t, 4>{0, 255, 0, 255},
            "later title overlay remains above the Avatar after present");
    require(pixel(14, 30) == std::array<uint8_t, 4>{0, 0, 0, 255} &&
                pixel(49, 30) == std::array<uint8_t, 4>{0, 0, 0, 255},
            "scissor clips the portions of both triangles inside the viewport but outside the scissor");
    for (uint32_t y = 0; y < 48; ++y) for (uint32_t x = 0; x < 64; ++x)
        if (x < 16 || x >= 48 || y < 12 || y >= 36)
            require(pixel(x, y) == std::array<uint8_t, 4>{0, 0, 0, 255},
                    "Avatar drawing does not spill outside the active view and scissor");

    // A scene surface nearer than the Avatar must occlude it. This catches
    // accidentally using (or clearing) a private depth buffer for the model.
    const auto require_occluded = [&] {
        const auto image = presentation.readback_color();
        require(image.size() == 64 * 48 * 4, "occlusion readback contains the complete target");
        for (size_t at = 0; at < image.size(); at += 4)
            require(image[at] == 0 && image[at + 1] == 0 && image[at + 2] == 0 && image[at + 3] == 255,
                    "nearer scene depth occludes the Avatar without being cleared by its pass");
    };
    frame.pose.bones[0].translation[0] = 0;
    clear.color_value = {0, 0, 0, 1};
    clear.depth = true;
    clear.depth_value = 0.25f;
    presentation.clear(clear);
    ++frame.present;
    presentation.draw_player_model(frame);
    require_occluded();

    if (presentation.raster_state().inverted_depth_supported()) {
        const plume::RenderViewport reversed(8, 8, 48, 32, 1, 0);
        presentation.set_raster_state(reversed, scissor);
        clear.depth_value = 0;
        presentation.clear(clear);
        ++frame.present;
        presentation.draw_player_model(frame);
        const auto reversed_pixels = presentation.readback_color();
        const size_t sample = (25 * 64 + 32) * 4;
        require(reversed_pixels.size() == 64 * 48 * 4 && reversed_pixels[sample] == 0 &&
                    reversed_pixels[sample + 1] == 0 && reversed_pixels[sample + 2] == 255,
                "reversed viewport draws Avatar against far depth zero using the reversed comparison");
        clear.depth_value = 0.75f;
        presentation.clear(clear);
        ++frame.present;
        presentation.draw_player_model(frame);
        require_occluded();
    }
}

// Exercise the real swap-chain path against a surface with fewer optional
// usages. A strict driver rejects unsupported flags instead of ignoring them.
struct RestrictedSurface {
    inline static PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR get_capabilities;
    inline static PFN_vkCreateSwapchainKHR create_swapchain;
    inline static PFN_vkCmdCopyImage copy_image;
    inline static VkImageUsageFlags allowed_usage;
    inline static VkImageUsageFlags reported_usage;
    inline static unsigned creates, rejected, copies;
    inline static unsigned queries, fail_query;

    static VKAPI_ATTR VkResult VKAPI_CALL capabilities(
        VkPhysicalDevice device, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* caps) {
        if (++queries == fail_query) return VK_ERROR_SURFACE_LOST_KHR;
        const auto result = get_capabilities(device, surface, caps);
        if (result == VK_SUCCESS) {
            caps->supportedUsageFlags &= allowed_usage;
            reported_usage = caps->supportedUsageFlags;
        }
        return result;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL create(
        VkDevice device, const VkSwapchainCreateInfoKHR* info,
        const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain) {
        ++creates;
        if (info->imageUsage & ~reported_usage) {
            ++rejected;
            return VK_ERROR_UNKNOWN;
        }
        return create_swapchain(device, info, allocator, swapchain);
    }
    static VKAPI_ATTR void VKAPI_CALL copy(
        VkCommandBuffer command, VkImage source, VkImageLayout source_layout,
        VkImage destination, VkImageLayout destination_layout, uint32_t count, const VkImageCopy* regions) {
        ++copies;
        copy_image(command, source, source_layout, destination, destination_layout, count, regions);
    }
    explicit RestrictedSurface(VkImageUsageFlags usage, unsigned fail = 0) {
        get_capabilities = vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
        create_swapchain = vkCreateSwapchainKHR;
        copy_image = vkCmdCopyImage;
        allowed_usage = usage;
        reported_usage = 0;
        creates = rejected = copies = 0;
        queries = 0;
        fail_query = fail;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR = capabilities;
        vkCreateSwapchainKHR = create;
        vkCmdCopyImage = copy;
    }
    ~RestrictedSurface() {
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR = get_capabilities;
        vkCreateSwapchainKHR = create_swapchain;
        vkCmdCopyImage = copy_image;
    }
};

void vulkan_swapchain_respects_surface_usage() {
    if (d3d12()) return;
    sfr::NativeGraphics graphics;
    graphics.initialize();
    for (const auto usage : {VkImageUsageFlags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT),
                            VkImageUsageFlags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)}) {
        RestrictedSurface surface(usage);
        sfr::NativePresentation presentation(graphics, 320, 240);
        require(RestrictedSurface::creates == 1 && RestrictedSurface::rejected == 0,
                "swap-chain creation only requests supported usages");
        sfr::NativeClear clear{};
        clear.color = true;
        clear.color_value = {1, 0, 0, 1};
        presentation.clear(clear);
        presentation.present();
        graphics.wait_idle();
        require(RestrictedSurface::copies == ((RestrictedSurface::reported_usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ? 1u : 0u),
                "presentation copies only when the swap-chain images support transfer destination usage");
    }
}

void vulkan_failed_surface_query_does_not_create_swapchain() {
    if (d3d12()) return;
    sfr::NativeGraphics graphics;
    graphics.initialize();
    // The wrapper's initial query succeeds; the query immediately before
    // creating the images fails, e.g. because the surface was lost.
    RestrictedSurface surface(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 2);
    require(rejects_with<std::runtime_error>([&] { sfr::NativePresentation p(graphics, 320, 240); }),
            "failed surface query stops presentation initialization");
    require(RestrictedSurface::creates == 0, "failed surface query never submits guessed capabilities to the driver");
}

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
        // Must precede any code that caches model_wanted() or model scale.
        avatar_draws_keep_viewport_order_and_independent_poses();
        vulkan_swapchain_respects_surface_usage();
        vulkan_failed_surface_query_does_not_create_swapchain();
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
