// The graphics start-up of the game without the game: the device, the
// presentation, the renderer, the game's pipelines and draws with each of
// them, and a few presented frames, each step written to game.log (and on
// Android to logcat) before it runs, so a device that dies in one of them
// (Issue #1: Mali-G57, silently, right after NATIVE_RENDER_SCALE) names the
// step. Firebase Test Lab starts it with a game-loop intent
// (GraphicsTestActivity), where there are no game files to start from; on a
// desktop SFR_GRAPHICS_SELFTEST=1 runs it instead of the game.
#include "native_graphics.h"
#include "native_presentation.h"
#include "native_renderer.h"
#include "graphics_selftest_shaders.h"
#include "pipeline_manifest.h"
#include "spirv_vertex_inputs.h"

#include "plume_vulkan.h"
#include <volk.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <vector>

namespace {
void step(const std::string& text) {
    std::fprintf(stderr, "GRAPHICS_SELFTEST %s\n", text.c_str());
    std::fflush(stderr);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "FreeRiders", "GRAPHICS_SELFTEST %s", text.c_str());
#endif
}

// What the game asks of the device that Adreno has and a Mali may not.
void describe(sfr::NativeGraphics& graphics) {
    if (graphics.backend() != sfr::GraphicsBackend::vulkan) return;
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
        "independent_blend=%u dual_source=%u logic_op=%u clip_distance=%u cull_distance=%u robust_buffer_access=%u "
        "sampled_image_array_dynamic_indexing=%u",
        properties.properties.deviceName, properties.properties.driverVersion,
        VK_API_VERSION_MAJOR(properties.properties.apiVersion), VK_API_VERSION_MINOR(properties.properties.apiVersion),
        VK_API_VERSION_PATCH(properties.properties.apiVersion), features.textureCompressionBC,
        features.textureCompressionETC2, features.textureCompressionASTC_LDR, features.geometryShader,
        features.fillModeNonSolid, features.depthClamp, features.independentBlend, features.dualSrcBlend, features.logicOp,
        features.shaderClipDistance, features.shaderCullDistance, features.robustBufferAccess,
        features.shaderSampledImageArrayDynamicIndexing);
    step(text);
    std::snprintf(text, sizeof(text),
        "limits stage_sampled_images=%u stage_samplers=%u stage_resources=%u set_sampled_images=%u set_samplers=%u "
        "bound_sets=%u push_constants=%u uab_stage_sampled_images=%u uab_stage_samplers=%u uab_set_sampled_images=%u "
        "uab_set_samplers=%u uab_pool=%u image_2d=%u uniform_range=%u storage_range=%u dynamic_uniform=%u "
        "stage_uniform=%u vertex_stride=%u vertex_offset=%u anisotropy=%.1f",
        limits.maxPerStageDescriptorSampledImages, limits.maxPerStageDescriptorSamplers,
        limits.maxPerStageResources, limits.maxDescriptorSetSampledImages, limits.maxDescriptorSetSamplers,
        limits.maxBoundDescriptorSets, limits.maxPushConstantsSize,
        indexing.maxPerStageDescriptorUpdateAfterBindSampledImages, indexing.maxPerStageDescriptorUpdateAfterBindSamplers,
        indexing.maxDescriptorSetUpdateAfterBindSampledImages, indexing.maxDescriptorSetUpdateAfterBindSamplers,
        indexing.maxUpdateAfterBindDescriptorsInAllPools, limits.maxImageDimension2D, limits.maxUniformBufferRange,
        limits.maxStorageBufferRange, limits.maxDescriptorSetUniformBuffersDynamic,
        limits.maxPerStageDescriptorUniformBuffers, limits.maxVertexInputBindingStride,
        limits.maxVertexInputAttributeOffset, double(limits.maxSamplerAnisotropy));
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

// Deterministic numbers for the draws' vertices and constants.
struct Random {
    uint64_t state = 0x9E3779B97F4A7C15ull;
    uint32_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return uint32_t(state >> 16);
    }
    float unit() { return float(next() % 20001) / 10000.0f - 1.0f; }  // [-1, 1]
};

// A draw with every pipeline the game prepared, as the game makes them
// (NativeRenderer::draw: the upload ring, the descriptor sets, the bounded
// layout's per-draw sets), with vertices and constants of numbers in
// [-1, 1] so some triangles cover pixels, and textures that are a plain
// one, the framebuffer's colour and its depth resolved, and the null ones.
// Each draw is submitted and waited for alone (SFR_SELFTEST_BATCH=n draws n
// at a time instead), so a GPU that faults or hangs names the pipeline.
// What a shader's SPIR-V holds that a GPU might trip on: bit 0 a loop, bit 1
// a switch, bit 2 a texture sample, bit 3 a discard.
uint32_t shader_traits(const sfr::ShaderCacheEntry* entry) {
    if (!entry) return 0;
    const auto code = entry->code(true);
    uint32_t traits = 0;
    for (size_t at = 20; at + 4 <= code.size();) {
        uint32_t word;
        std::memcpy(&word, code.data() + at, 4);
        const uint32_t count = word >> 16, opcode = word & 0xffff;
        if (!count) break;
        if (opcode == 246) traits |= 1;                    // OpLoopMerge
        if (opcode == 251) traits |= 2;                    // OpSwitch
        if (opcode >= 87 && opcode <= 94) traits |= 4;     // OpImageSample*
        if (opcode == 252) traits |= 8;                    // OpKill
        at += size_t(count) * 4;
    }
    return traits;
}

bool setting_is(const char* name, const char* value) {
    const char* text = std::getenv(name);
    return text && std::strcmp(text, value) == 0;
}

bool draw_pipelines(sfr::NativeGraphics& graphics, sfr::NativePresentation& presentation, sfr::NativeRenderer& renderer) {
    auto draws = renderer.prepared_draws();
    // SFR_SELFTEST_ORDER=class: the draws whose shaders hold the least go
    // first (by shader_traits, the vertex shader's above the pixel
    // shader's), so where a GPU hangs tells which kind of shader does it.
    std::vector<uint32_t> classes(draws.size());
    for (size_t i = 0; i < draws.size(); ++i)
        classes[i] = shader_traits(draws[i].vertex_entry) << 4 | shader_traits(draws[i].pixel_entry);
    if (setting_is("SFR_SELFTEST_ORDER", "class")) {
        std::vector<size_t> order(draws.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return classes[a] < classes[b]; });
        std::vector<sfr::NativeDraw> sorted;
        std::vector<uint32_t> sorted_classes;
        for (const size_t i : order) {
            sorted.push_back(draws[i]);
            sorted_classes.push_back(classes[i]);
        }
        draws = std::move(sorted);
        classes = std::move(sorted_classes);
    }
    // SFR_SELFTEST_VS=trivial|few|uniform and SFR_SELFTEST_PS=trivial|sample:
    // stand-in shaders (graphics_selftest_shaders.h) in place of the game's,
    // with the rest of each draw as it was: next to nothing with eighteen
    // outputs or four, the vertex constants read, a texture sampled through
    // the descriptor arrays. SFR_SELFTEST_NO_ELEMENTS=1 drops the vertex
    // inputs and SFR_SELFTEST_NO_DEPTH=1 the depth, stencil and blend.
    auto& device = graphics.device();
    struct Choice { const char* name; const uint32_t* code; size_t bytes; };
    const auto stand_in = [&](const char* setting, std::initializer_list<Choice> choices)
        -> std::unique_ptr<plume::RenderShader> {
        for (const auto& choice : choices)
            if (setting_is(setting, choice.name))
                return device.createShader(choice.code, choice.bytes, "shaderMain", plume::RenderShaderFormat::SPIRV);
        return {};
    };
    const auto stand_in_vs = stand_in("SFR_SELFTEST_VS", {
        {"trivial", selftest_trivial_vs, sizeof(selftest_trivial_vs)},
        {"few", selftest_few_outputs_vs, sizeof(selftest_few_outputs_vs)},
        {"uniform", selftest_uniform_vs, sizeof(selftest_uniform_vs)}});
    const auto stand_in_ps = stand_in("SFR_SELFTEST_PS", {
        {"trivial", selftest_trivial_ps, sizeof(selftest_trivial_ps)},
        {"sample", selftest_sample_ps, sizeof(selftest_sample_ps)}});
    const bool trivial_vs = stand_in_vs != nullptr, trivial_ps = stand_in_ps != nullptr;
    // The stand-ins read no vertex inputs: on a device short of attributes
    // their pipelines declare none (spirv_vertex_inputs.h).
    if (stand_in_vs && graphics.compacts_vertex_inputs()) sfr::VertexInputUse::note(stand_in_vs.get(), {});
    for (auto& draw : draws) {
        // Without the shaders' entries the renderer makes a pipeline anew.
        if (trivial_vs) draw.vertex_shader = stand_in_vs.get();
        if (trivial_ps) draw.pixel_shader = stand_in_ps.get();
        if (trivial_vs || trivial_ps) draw.vertex_entry = draw.pixel_entry = nullptr;
        if (setting_is("SFR_SELFTEST_NO_ELEMENTS", "1")) draw.elements.clear();
        if (setting_is("SFR_SELFTEST_NO_DEPTH", "1")) {
            draw.depth_enabled = draw.depth_write = draw.stencil_enabled = false;
            draw.blend.blendEnabled = false;
        }
    }
    // SFR_SELFTEST_LIMIT=n: only the first n draws.
    if (const char* limit = std::getenv("SFR_SELFTEST_LIMIT"); limit && std::atoi(limit) > 0 &&
        size_t(std::atoi(limit)) < draws.size()) {
        draws.resize(size_t(std::atoi(limit)));
        classes.resize(draws.size());
    }
    const char* batch_setting = std::getenv("SFR_SELFTEST_BATCH");
    const size_t batch = (std::max)(1, batch_setting ? std::atoi(batch_setting) : 1);
    // SFR_SELFTEST_TEXTURES: mixed (the default), plain (no resolved ones),
    // nodepth (no resolved depth) or null (only the zero-reading ones).
    const char* textures_setting = std::getenv("SFR_SELFTEST_TEXTURES");
    const std::string textures = textures_setting && *textures_setting ? textures_setting : "mixed";
    step("draws begin pipelines=" + std::to_string(draws.size()) + " batch=" + std::to_string(batch) +
         " textures=" + textures);
    const float width = float(presentation.width()), height = float(presentation.height());
    presentation.set_raster_state(plume::RenderViewport(0.0f, 0.0f, width, height),
                                  plume::RenderRect(0, 0, int32_t(width), int32_t(height)));
    presentation.clear(sfr::NativeClear{.color = true, .depth = true, .stencil = true, .color_value = {0.2f, 0.3f, 0.4f, 1.0f},
                                        .depth_value = 0.5f});
    const uint32_t plain = renderer.placeholder_texture();
    step("draws resolve color");
    const uint32_t color = renderer.adopt_resolved_target(0x01000000u);
    step("draws resolve depth");
    const uint32_t depth = renderer.adopt_resolved_target(0x01400000u, true);
    presentation.flush();
    step("draws textures plain=" + std::to_string(plain) + " color=" + std::to_string(color) + " depth=" +
         std::to_string(depth));
    Random random;
    std::vector<uint8_t> vertices;
    double slowest = 0;
    size_t slowest_index = 0;
    for (size_t index = 0; index < draws.size(); ++index) {
        auto& draw = draws[index];
        draw.vertex_count = 96;
        const uint32_t stride = (std::max)(draw.stride, 4u);
        vertices.resize(size_t(draw.vertex_count) * stride);
        for (size_t at = 0; at + 4 <= vertices.size(); at += 4) {
            const float value = random.unit();
            std::memcpy(vertices.data() + at, &value, 4);
        }
        draw.vertices = vertices;
        for (auto& word : draw.vertex_constants) {
            const float value = random.unit();
            std::memcpy(&word, &value, 4);
        }
        for (auto& word : draw.pixel_constants) {
            const float value = random.unit();
            std::memcpy(&word, &value, 4);
        }
        for (uint32_t i = 0; i < 16; ++i) {
            // count, start, step (Xenos loop constants, as the game unpacks them)
            draw.loop_constants[i * 4 + 0] = 4;
            draw.loop_constants[i * 4 + 1] = 0;
            draw.loop_constants[i * 4 + 2] = 1;
            draw.loop_constants[i * 4 + 3] = 0;
            const uint32_t pick = uint32_t(index + i) % 5;
            draw.shared.texture_2d[i] = pick == 0 ? plain : pick == 1 ? color : pick == 2 ? depth : pick == 3 ? plain : 0;
            draw.shared.texture_2d_array[i] = 1;
            draw.shared.texture_cube[i] = 2;
            draw.shared.sampler[i] = 0;
        }
        draw.shared.booleans = random.next();
        draw.shared.alpha_threshold = 0.5f;
        if (textures == "null" || textures == "plain" || textures == "nodepth")
            for (uint32_t i = 0; i < 16; ++i) {
                auto& texture = draw.shared.texture_2d[i];
                if (textures == "null") texture = 0;
                else if (textures == "plain" && texture) texture = plain;
                else if (textures == "nodepth" && texture == depth) texture = plain;
            }
        const auto hash = [](const sfr::ShaderCacheEntry* entry) {
            return static_cast<unsigned long long>(entry ? sfr::pipeline_shader_id(entry->source).hash : 0);
        };
        char text[320];
        std::snprintf(text, sizeof(text),
                      "draw %zu class=%02x vs=%016llx ps=%016llx topology=%d stride=%u elements=%zu depth=%d stencil=%d "
                      "blend=%d",
                      index, classes[index], hash(draw.vertex_entry), hash(draw.pixel_entry),
                      int(draw.topology), draw.stride, draw.elements.size(), int(draw.depth_enabled),
                      int(draw.stencil_enabled), int(draw.blend.blendEnabled));
        step(text);
        renderer.draw(draw);
        if ((index + 1) % batch && index + 1 != draws.size()) continue;
        const auto started = std::chrono::steady_clock::now();
        presentation.flush();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (ms > slowest) {
            slowest = ms;
            slowest_index = index;
        }
        // A fence that waited seconds: the GPU hung (a lost device follows).
        if (ms > 3000) {
            step("draws hung at=" + std::to_string(index) + " ms=" + std::to_string(int(ms)));
            return false;
        }
        if ((index + 1) % (batch * 16) == 0) presentation.present();
    }
    // How much of the frame the draws covered: the pixels no longer the clear colour.
    const auto pixels = presentation.readback_color();
    size_t drawn = 0;
    for (size_t at = 0; at + 4 <= pixels.size(); at += 4)
        drawn += pixels[at] != 102 || pixels[at + 1] != 77 || pixels[at + 2] != 51;
    presentation.present();
    step("draws end slowest_ms=" + std::to_string(slowest) + " at=" + std::to_string(slowest_index) +
         " drawn_pixels=" + std::to_string(drawn) + " of " + std::to_string(pixels.size() / 4));
    return true;
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
        // With the game's shaders beside it (GraphicsTestActivity copies the
        // APK's shaders.pack and pipeline manifest), every pipeline the game
        // prepares at start, where Mali crashed (Issues #166, #169), and a
        // draw with each, where an old Mali lost its device (Issue #169).
        bool drawn = true;
        const char* pack = std::getenv("SFR_SHADER_PACK");
        if (std::filesystem::exists(pack && *pack ? pack : "shaders.pack")) {
            step("pipelines");
            renderer.prepare_pipelines();
            step("pipelines done");
            const char* draws = std::getenv("SFR_SELFTEST_DRAWS");
            if (!draws || *draws != '0') drawn = draw_pipelines(graphics, presentation, renderer);
        }
        if (!drawn) {
            step("end ok=0 error=a draw hung the GPU");
            return 3;
        }
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
