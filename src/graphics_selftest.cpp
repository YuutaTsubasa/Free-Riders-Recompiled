// The graphics start-up of the game without the game (Android): the device,
// the presentation, the renderer and a few presented frames, each step
// written to game.log and to logcat before it runs, so a device that dies in
// one of them (Issue #1: Mali-G57, silently, right after NATIVE_RENDER_SCALE)
// names the step. Firebase Test Lab starts it with a game-loop intent
// (GraphicsTestActivity), where there are no game files to start from.
#include "native_graphics.h"
#include "native_presentation.h"
#include "native_renderer.h"

#include "plume_vulkan.h"
#include <volk.h>

#include <android/log.h>
#include <cstdio>
#include <exception>
#include <string>

namespace {
void step(const std::string& text) {
    std::fprintf(stderr, "GRAPHICS_SELFTEST %s\n", text.c_str());
    std::fflush(stderr);
    __android_log_print(ANDROID_LOG_INFO, "FreeRiders", "GRAPHICS_SELFTEST %s", text.c_str());
}

// What the game asks of the device that Adreno has and a Mali may not.
void describe(sfr::NativeGraphics& graphics) {
    auto& device = static_cast<plume::VulkanDevice&>(graphics.device());
    const VkPhysicalDevice physical = device.physicalDevice;
    VkPhysicalDeviceDescriptorIndexingProperties indexing{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &indexing};
    vkGetPhysicalDeviceProperties2(physical, &properties);
    const auto& limits = properties.properties.limits;
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(physical, &features);
    char text[1024];
    std::snprintf(text, sizeof(text),
        "device=%s driver=0x%x api=%u.%u.%u bc=%u etc2=%u astc=%u geometry=%u fill_non_solid=%u depth_clamp=%u "
        "independent_blend=%u dual_source=%u logic_op=%u",
        properties.properties.deviceName, properties.properties.driverVersion,
        VK_API_VERSION_MAJOR(properties.properties.apiVersion), VK_API_VERSION_MINOR(properties.properties.apiVersion),
        VK_API_VERSION_PATCH(properties.properties.apiVersion), features.textureCompressionBC,
        features.textureCompressionETC2, features.textureCompressionASTC_LDR, features.geometryShader,
        features.fillModeNonSolid, features.depthClamp, features.independentBlend, features.dualSrcBlend, features.logicOp);
    step(text);
    std::snprintf(text, sizeof(text),
        "limits stage_sampled_images=%u stage_samplers=%u stage_resources=%u set_sampled_images=%u set_samplers=%u "
        "bound_sets=%u push_constants=%u uab_stage_sampled_images=%u uab_stage_samplers=%u uab_set_sampled_images=%u "
        "uab_set_samplers=%u uab_pool=%u image_2d=%u",
        limits.maxPerStageDescriptorSampledImages, limits.maxPerStageDescriptorSamplers,
        limits.maxPerStageResources, limits.maxDescriptorSetSampledImages, limits.maxDescriptorSetSamplers,
        limits.maxBoundDescriptorSets, limits.maxPushConstantsSize,
        indexing.maxPerStageDescriptorUpdateAfterBindSampledImages, indexing.maxPerStageDescriptorUpdateAfterBindSamplers,
        indexing.maxDescriptorSetUpdateAfterBindSampledImages, indexing.maxDescriptorSetUpdateAfterBindSamplers,
        indexing.maxUpdateAfterBindDescriptorsInAllPools, limits.maxImageDimension2D);
    step(text);
    for (auto [name, format] : {std::pair{"D32S8", VK_FORMAT_D32_SFLOAT_S8_UINT}, std::pair{"D24S8", VK_FORMAT_D24_UNORM_S8_UINT},
                                std::pair{"BGRA8", VK_FORMAT_B8G8R8A8_UNORM}, std::pair{"RGBA8", VK_FORMAT_R8G8B8A8_UNORM},
                                std::pair{"BC1", VK_FORMAT_BC1_RGBA_UNORM_BLOCK}, std::pair{"BC3", VK_FORMAT_BC3_UNORM_BLOCK}}) {
        VkFormatProperties format_properties{};
        vkGetPhysicalDeviceFormatProperties(physical, format, &format_properties);
        std::snprintf(text, sizeof(text), "format %s optimal=0x%x", name, format_properties.optimalTilingFeatures);
        step(text);
    }
}
}

int sfr_graphics_selftest() {
    try {
        step("begin");
        sfr::NativeGraphics graphics;
        step("graphics.initialize");
        graphics.initialize();
        describe(graphics);
        step("presentation 1280x720");
        sfr::NativePresentation presentation(graphics, 1280, 720);
        step("renderer");
        sfr::NativeRenderer renderer(graphics, presentation);
        bool cleared = false;
        for (int frame = 0; frame < 120; ++frame) {
            if (frame == 0) step("first clear and present");
            sfr::NativeClear clear;
            clear.color = true;
            clear.color_value = {float(frame % 60) / 60.0f, 0.4f, 0.8f, 1.0f};
            presentation.clear(clear);
            if (frame == 60) {
                step("readback");
                const auto pixels = presentation.readback_color();
                // BGRA: blue is 0.8 of 255.
                cleared = pixels.size() >= 4 && pixels[0] > 190 && pixels[0] < 220;
            }
            presentation.present();
        }
        step(std::string("end ok=1 readback=") + (cleared ? "1" : "0"));
        return 0;
    } catch (const std::exception& error) {
        step(std::string("end ok=0 error=") + error.what());
        return 2;
    }
}
