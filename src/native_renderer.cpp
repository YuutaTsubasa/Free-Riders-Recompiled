#include "native_renderer.h"
#include "native_pipeline_key.h"
#include "native_constant_upload.h"
#include "native_upload_batch.h"
#include "pipeline_manifest.h"
#include "runtime_shader_cache.h"
#include <thread>
#include <exception>
#ifdef _WIN32
#include "plume_d3d12.h"
#endif
#include "plume_vulkan.h"
#include "vulkan_shader_source.h"
#include "guest_memory.h"
#include "native_formats.h"
#include "native_graphics.h"
#include "native_presentation.h"
#include <plume_render_interface.h>
#include <plume_render_interface_builders.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wrl/client.h>
#include <dxcapi.h>
#endif

namespace sfr {
namespace {
// Same binding model as Marathon Recompiled's D3D12 backend for XenosRecomp
// shaders: one bindless texture set bound as spaces 0..2 (2D, 2D array, cube),
// samplers in space 3, the survey UAV in space 4, and b0..b2 in space 4.
constexpr uint32_t texture_capacity = 4096, sampler_capacity = 256;
constexpr uint32_t null_2d = 0, null_2d_array = 1, null_cube = 2, first_texture = 3;
constexpr uint32_t zero_slot = 15;
constexpr uint64_t ring_size = 64ull << 20;
// The skinning palette constant buffer, as vertex_palette.cpp declares it.
constexpr uint64_t palette_bytes = 1024 * 16;

[[noreturn]] void unsupported(uint32_t value, const std::string& detail) {
    throw RuntimeStop("native-draw", value, detail);
}

uint64_t align(uint64_t value, uint64_t alignment) { return (value + alignment - 1) / alignment * alignment; }

// Content hash of guest texture bytes (checked range, read directly).
uint64_t content_hash(const uint8_t* data, uint64_t size) {
    uint64_t hash = 0xcbf29ce484222325ull ^ size;
    uint64_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t word;
        std::memcpy(&word, data + i, 8);
        hash = (hash ^ word) * 0x100000001b3ull;
        hash ^= hash >> 29;
    }
    for (; i < size; ++i) hash = (hash ^ data[i]) * 0x100000001b3ull;
    return hash;
}

#ifdef _WIN32
// Links XenosRecomp pixel-shader libraries with their specialization constants
// using the DXC that built the shader cache (DXC links no libraries of another
// version): XenosRecomp's dxc-bin in the checkout (the working directory), a
// release's own copy in SFR_DXC_LIBRARY (the directory holding dxcompiler.dll).
class DxcLinker {
public:
    DxcLinker() {
        std::wstring directory =
            (std::filesystem::current_path() / L"tools\\XenosRecomp\\thirdparty\\dxc-bin\\bin\\x64\\").wstring();
        if (const wchar_t* chosen = _wgetenv(L"SFR_DXC_LIBRARY"); chosen && *chosen)
            directory = std::wstring(chosen) + L"\\";
        // dxcompiler loads dxil.dll by name for signing; load it first from the same directory.
        LoadLibraryW((directory + L"dxil.dll").c_str());
        module_ = LoadLibraryW((directory + L"dxcompiler.dll").c_str());
        if (!module_) unsupported(GetLastError(), "dxcompiler.dll is unavailable");
        auto create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(module_, "DxcCreateInstance"));
        if (!create || FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(compiler_.GetAddressOf()))) ||
            FAILED(create(CLSID_DxcLinker, IID_PPV_ARGS(linker_.GetAddressOf()))) ||
            FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(utils_.GetAddressOf()))))
            unsupported(0, "DXC compiler, linker or utilities are unavailable");
    }
    std::vector<uint8_t> link(std::span<const uint8_t> library, uint32_t spec_constants) {
        const std::string hlsl = "export uint g_SpecConstants() { return " + std::to_string(spec_constants) + "; }";
        DxcBuffer source{hlsl.data(), hlsl.size(), DXC_CP_ACP};
        const wchar_t* arguments[] = {L"-T", L"lib_6_3"};
        Microsoft::WRL::ComPtr<IDxcResult> compiled;
        Microsoft::WRL::ComPtr<IDxcBlob> spec;
        HRESULT status = S_OK;
        if (FAILED(compiler_->Compile(&source, arguments, 2, nullptr, IID_PPV_ARGS(compiled.GetAddressOf()))) ||
            FAILED(compiled->GetStatus(&status)) || FAILED(status) ||
            FAILED(compiled->GetResult(spec.GetAddressOf())))
            unsupported(spec_constants, "specialization-constant library did not compile");
        Microsoft::WRL::ComPtr<IDxcBlobEncoding> shader;
        if (FAILED(utils_->CreateBlob(library.data(), uint32_t(library.size()), DXC_CP_ACP, shader.GetAddressOf())))
            unsupported(0, "shader library blob creation failed");
        const std::wstring spec_name = L"SpecConstants_" + std::to_wstring(spec_constants);
        const std::wstring shader_name = L"Shader_" + std::to_wstring(++libraries_);
        linker_->RegisterLibrary(spec_name.c_str(), spec.Get());
        linker_->RegisterLibrary(shader_name.c_str(), shader.Get());
        const wchar_t* names[] = {spec_name.c_str(), shader_name.c_str()};
        Microsoft::WRL::ComPtr<IDxcOperationResult> linked;
        Microsoft::WRL::ComPtr<IDxcBlob> output;
        if (FAILED(linker_->Link(L"shaderMain", L"ps_6_0", names, 2, nullptr, 0, linked.GetAddressOf())) ||
            FAILED(linked->GetStatus(&status)) || FAILED(status) || FAILED(linked->GetResult(output.GetAddressOf())) || !output) {
            Microsoft::WRL::ComPtr<IDxcBlobEncoding> errors;
            if (linked) linked->GetErrorBuffer(errors.GetAddressOf());
            std::string message = "pixel shader link failed";
            if (errors && errors->GetBufferSize())
                message += ": " + std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
            unsupported(spec_constants, message);
        }
        const auto* bytes = static_cast<const uint8_t*>(output->GetBufferPointer());
        return {bytes, bytes + output->GetBufferSize()};
    }
private:
    HMODULE module_ = nullptr;
    Microsoft::WRL::ComPtr<IDxcCompiler3> compiler_;
    Microsoft::WRL::ComPtr<IDxcLinker> linker_;
    Microsoft::WRL::ComPtr<IDxcUtils> utils_;
    uint32_t libraries_ = 0;
};
#endif

std::unique_ptr<plume::RenderPipeline> create_draw_pipeline(plume::RenderDevice& device,
        const plume::RenderPipelineLayout* layout, const NativeDraw& draw, GraphicsBackend backend) {
    const std::array<plume::RenderInputSlot, 2> slots{plume::RenderInputSlot(0, draw.stride),
                                                       plume::RenderInputSlot(zero_slot, 0)};
        plume::RenderGraphicsPipelineDesc desc;
        desc.pipelineLayout = layout;
        desc.vertexShader = draw.vertex_shader;
        desc.pixelShader = draw.pixel_shader;
        const plume::RenderSpecConstant spec(0, draw.pixel_spec_constants);
        if (draw.pixel_spec_constants) {
            desc.specConstants = &spec;
            desc.specConstantsCount = 1;
        }
        desc.primitiveTopology = draw.topology;
        desc.cullMode = draw.cull;
        desc.depthEnabled = draw.depth_enabled;
        desc.depthWriteEnabled = draw.depth_write;
        desc.depthFunction = draw.depth_function;
        desc.stencilEnabled = draw.stencil_enabled;
        if (draw.stencil_enabled) {
            desc.stencilReference = draw.stencil_reference;
            desc.stencilReadMask = draw.stencil_read_mask;
            desc.stencilWriteMask = draw.stencil_write_mask;
            desc.stencilFrontFace = draw.stencil_front;
            desc.stencilBackFace = draw.stencil_back;
        }
        desc.depthTargetFormat = plume::RenderFormat::D32_FLOAT_S8_UINT;
        desc.renderTargetCount = 1;
        desc.renderTargetFormat[0] = plume::RenderFormat::B8G8R8A8_UNORM;
        desc.renderTargetBlend[0] = draw.blend.description(draw.write_mask);
        desc.inputSlots = slots.data();
        desc.inputSlotsCount = uint32_t(slots.size());
        desc.inputElements = draw.elements.data();
        desc.inputElementsCount = uint32_t(draw.elements.size());
        auto pipeline = device.createGraphicsPipeline(desc);
        if (!pipeline) return {};
        if (backend == GraphicsBackend::vulkan) {
            if (static_cast<plume::VulkanGraphicsPipeline*>(pipeline.get())->vk == VK_NULL_HANDLE) return {};
        }
#ifdef _WIN32
        else if (!static_cast<plume::D3D12GraphicsPipeline*>(pipeline.get())->d3d) return {};
#endif
        return pipeline;
}

void report_vulkan_formats(NativeGraphics& graphics);

// Most phone GPUs have no BC (DXT) formats; their textures are decoded to
// RGBA8 on the CPU. SFR_DECODE_BC=1 forces that path where BC exists.
bool block_compression_supported(NativeGraphics& graphics) {
    static const bool supported = [&] {
        if (const char* forced = std::getenv("SFR_DECODE_BC"); forced && *forced == '1') return false;
        if (graphics.backend() != GraphicsBackend::vulkan) return true;
        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeatures(static_cast<plume::VulkanDevice&>(graphics.device()).physicalDevice, &features);
        return features.textureCompressionBC == VK_TRUE;
    }();
    static const bool reported = [&] {
        std::cerr << "NATIVE_TEXTURE_BC supported=" << supported << '\n';
        report_vulkan_formats(graphics);
        return true;
    }();
    (void)reported;
    return supported;
}

// What the Vulkan driver supports of the formats the game needs, logged once.
// A format a driver cannot sample or fetch shows up as wrong colours or
// missing geometry, so a report from an unfamiliar device says in one line
// whether that is the cause. (An Adreno 750 supports all of them.)
void report_vulkan_formats(NativeGraphics& graphics) {
    if (graphics.backend() != GraphicsBackend::vulkan) return;
    const VkPhysicalDevice physical = static_cast<plume::VulkanDevice&>(graphics.device()).physicalDevice;
    struct Entry { const char* name; VkFormat format; VkFormatFeatureFlags needed; };
    static const Entry entries[] = {
        {"BC1", VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT},
        {"BC2", VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT},
        {"BC3", VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT},
        {"RGBA8", VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT},
        {"BGRA8", VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT},
        {"BGRA8 target", VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT},
        {"R8", VK_FORMAT_R8_UNORM, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT},
        {"RGBA16F", VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT},
        {"D32S8 target", VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT},
        {"vertex SHORT4N", VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT},
        {"vertex SHORT2N", VK_FORMAT_R16G16_SNORM, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT},
        {"vertex USHORT4N", VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT},
        {"vertex USHORT2N", VK_FORMAT_R16G16_UNORM, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT},
        {"vertex UBYTE4", VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT},
        {"vertex SHORT4", VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT},
        {"vertex FLOAT16_4", VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT},
        {"vertex FLOAT3", VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT},
    };
    std::string missing;
    for (const auto& entry : entries) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physical, entry.format, &properties);
        const VkFormatFeatureFlags features = (entry.needed & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)
            ? properties.bufferFeatures : properties.optimalTilingFeatures;
        if (!(features & entry.needed)) missing += std::string(missing.empty() ? "" : ", ") + entry.name;
    }
    std::cerr << "NATIVE_FORMATS missing=" << (missing.empty() ? "none" : missing) << '\n';
}

// A guest GPU physical address as a readable virtual address. Physical
// allocations are mapped in the 0xE0000000 view (GPU address + 4 KiB) or the
// 0xA0000000/0xC0000000 views.
uint32_t guest_view(GuestMemory& memory, uint32_t physical, uint64_t size) {
    for (uint64_t candidate : {uint64_t(physical) + 0xE0000000u - 0x1000u, uint64_t(physical) | 0xA0000000u,
                               uint64_t(physical) | 0xC0000000u}) {
        if (candidate > 0xFFFFFFFFu) continue;
        // Asked rather than thrown: this runs for every texture of every draw
        // and an exception costs microseconds (docs/performance.md).
        if (memory.readable(candidate, size)) return uint32_t(candidate);
    }
    unsupported(physical, "texture data is not mapped in any physical view");
}
}

struct NativeRenderer::Impl {
    // What the presentation's current command list has bound (its
    // list_generation): the layout, sets and zero stream stay bound from draw
    // to draw, so only the first draw of a list binds them, and the pipeline
    // is set only when it changes. Rebinding them all for every draw cost
    // more than a millisecond a race frame on D3D12 (a root signature
    // change drops every root argument).
    uint64_t bound_generation = ~0ull;
    const plume::RenderPipeline* bound_pipeline = nullptr;
    NativeGraphics& graphics;
    NativePresentation& presentation;
    std::unique_ptr<plume::RenderPipelineLayout> layout;
    std::unique_ptr<plume::RenderDescriptorSet> textures, samplers, survey;
    std::unique_ptr<plume::RenderBuffer> survey_buffer, zero_buffer;
    std::vector<std::unique_ptr<plume::RenderTexture>> texture_objects;
    std::vector<std::unique_ptr<plume::RenderTextureView>> texture_views;
    std::vector<std::unique_ptr<plume::RenderSampler>> sampler_objects;
    std::map<std::array<uint32_t, 4>, uint32_t> texture_indices;
    // Guest physical range of each cached texture, for invalidation on CPU writes.
    // dynamic: the CPU has rewritten this memory before (lock or watched
    // write); such textures are compared by content hash at their first use
    // in each frame (the planes may be write-combined, uncached memory).
    struct TextureRange { std::array<uint32_t, 4> key; uint64_t begin, end; uint64_t hash; bool dynamic;
                          uint64_t checked_frame = 0; uint64_t seen_frame = 0; };
    uint64_t frame = 1;  // counts flushes
    // Opt-in probe only: compare CPU bytes, never read write-combined uploads.
    // Actual uploads and bindings remain unchanged while measuring reuse.
    struct ConstantReuseProbe {
        std::array<std::array<uint32_t, 1024>, 2> previous{};
        bool valid = false;
        uint64_t uploaded = 0, reusable = 0;
    };
    std::unique_ptr<ConstantReuseProbe> constant_reuse_probe;
    std::unique_ptr<std::array<NativeConstantUpload, 2>> constant_uploads;
    uint64_t constant_saved_bytes = 0;
    std::set<uint64_t> dynamic_ranges;  // physical starts of rewritten textures
    std::map<uint32_t, TextureRange> texture_ranges;  // by descriptor index
    // Destination address of a resolve to its descriptor index (the copy of
    // the framebuffer the title samples afterwards).
    std::map<uint32_t, uint32_t> resolved_targets;
    std::bitset<texture_capacity> resolved_texture_indices;
    // Changes whenever texture() could answer the same fetch words
    // differently: a new frame, a resolve, a texture made or dropped.
    uint64_t texture_generation = 0;
    std::optional<uint32_t> placeholder;  // flat white texture (investigation)
    std::vector<uint32_t> free_texture_indices;
    uint32_t next_texture_index = 0;
    std::map<std::array<uint32_t, 2>, uint32_t> sampler_indices;
    std::unordered_map<uint64_t, std::unique_ptr<plume::RenderShader>> linked;
    // Hashed: an ordered map compared ~100-byte keys byte by byte down the
    // tree for every draw (3% of a race frame's main thread).
    struct KeyHash {
        size_t operator()(const std::vector<uint8_t>& key) const noexcept {
            uint64_t hash = 0xcbf29ce484222325ull;
            size_t i = 0;
            for (; i + 8 <= key.size(); i += 8) {
                uint64_t word;
                std::memcpy(&word, key.data() + i, 8);
                hash = (hash ^ word) * 0x100000001b3ull;
                hash ^= hash >> 29;
            }
            for (; i < key.size(); ++i) hash = (hash ^ key[i]) * 0x100000001b3ull;
            return size_t(hash);
        }
    };
    std::unordered_map<std::vector<uint8_t>, std::shared_ptr<plume::RenderPipeline>, KeyHash> pipelines;
    std::unordered_map<std::vector<uint8_t>, std::shared_ptr<plume::RenderPipeline>, KeyHash> recipe_pipelines;
    std::map<std::vector<uint8_t>, PipelineRecipe> recipes;
    std::unordered_map<const ShaderCacheEntry*, std::unique_ptr<plume::RenderShader>> prepared_shaders;
    std::filesystem::path learned_manifest;
    bool manifest_dirty = false, preparation_started = false;
    std::chrono::steady_clock::time_point manifest_saved_at{};
    uint64_t prewarm_hits = 0;
    void save_manifest() noexcept;
    // Texture uploads are submitted without waiting, so their command lists
    // rotate: a list is only recorded again once the submission that used it
    // has finished. Kept for the legacy upload diagnostic path.
    static constexpr uint32_t upload_lists = 4;
    struct Upload {
        std::unique_ptr<plume::RenderCommandList> list;
        std::unique_ptr<plume::RenderCommandFence> fence;
        bool in_flight = false;
        // The staging buffers of the submission this slot last made: freed
        // when the slot is waited for, which is the only moment the GPU is
        // known to have finished reading them.
        std::vector<std::unique_ptr<plume::RenderBuffer>> staging;
    };
    std::array<Upload, upload_lists> uploads;
    uint32_t upload_slot = 0;
    std::unique_ptr<NativeUploadBatch> upload_batch;
    uint64_t batch_submissions_seen = 0;
    double batch_wait_ms_seen = 0;
    std::vector<std::unique_ptr<plume::RenderBuffer>> retired;
#ifdef _WIN32
    std::unique_ptr<DxcLinker> dxc;
#endif
    uint32_t draws = 0;
    uint32_t pipelines_created = 0;
    double pipeline_ms = 0;
    uint32_t ring_flushes = 0, textures_uploaded = 0;
    double texture_ms = 0;
    // Two upload rings: while the GPU renders one frame from one, the next
    // frame fills the other (NativePresentation::after_flush).
    std::unique_ptr<plume::RenderBuffer> rings[2];
    uint8_t* rings_mapped[2] = {};
    int ring_index = 0;
    uint64_t ring_offset = 0;
    // vertex_cache's entries, by guest address (first readable view), size
    // and layout; buffers are never changed once filled, and ones dropped are
    // kept until the frames that may read them have finished.
    struct VertexKey {
        uint32_t address; uint64_t bytes, layout;
        bool operator==(const VertexKey&) const = default;
    };
    struct VertexKeyHash {
        size_t operator()(const VertexKey& k) const noexcept {
            return size_t((uint64_t(k.address) * 0x9E3779B97F4A7C15ull) ^ (k.bytes * 0xC2B2AE3D27D4EB4Full) ^ k.layout);
        }
    };
    struct VertexEntry {
        std::unique_ptr<plume::RenderBuffer> buffer;
        std::array<uint32_t, 3> views{};
        uint32_t view_count = 0;
        uint32_t epoch = 0;
        uint64_t last_frame = 0, size = 0;
    };
    std::unordered_map<VertexKey, VertexEntry, VertexKeyHash> vertex_entries;
    std::vector<std::pair<uint64_t, std::unique_ptr<plume::RenderBuffer>>> retired_vertex_buffers;
    uint64_t cached_vertex_bytes = 0;
    void retire_vertices(VertexEntry& entry) {
        if (!entry.buffer) return;
        cached_vertex_bytes -= entry.size;
        retired_vertex_buffers.emplace_back(frame, std::move(entry.buffer));
    }
    // vertex_space's reservation: where in which ring, and its size.
    const uint8_t* reserved = nullptr;
    uint64_t reserved_offset = 0;
    int reserved_ring = 0;
    // Texture slots released during a frame stay reserved until the frame's
    // commands, which may still reference them, have completed; those of a
    // frame still in flight wait one submission more.
    std::vector<uint32_t> released_texture_indices, in_flight_texture_indices;

    Impl(NativeGraphics& g, NativePresentation& p) : graphics(g), presentation(p) {}
};

void NativeRenderer::Impl::save_manifest() noexcept {
    manifest_saved_at = std::chrono::steady_clock::now();
    if (!manifest_dirty || learned_manifest.empty()) return;
    try {
        std::vector<PipelineRecipe> entries;
        entries.reserve(recipes.size());
        for (const auto& [key, recipe] : recipes) entries.push_back(recipe);
        const bool saved = save_pipeline_manifest_file(learned_manifest, entries, uint32_t(graphics.backend()));
        if (saved) manifest_dirty = false;
        std::cerr << "PIPELINE_MANIFEST saved=" << saved << " recipes=" << entries.size()
                  << " path=" << learned_manifest.generic_string() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "PIPELINE_MANIFEST save_failed=" << error.what() << '\n';
    }
}

void NativeRenderer::prepare_pipelines() {
    if (impl_->preparation_started) return;
    impl_->preparation_started = true;
    const uint32_t backend = uint32_t(impl_->graphics.backend());
    const char* name = backend == 1 ? "vulkan" : "d3d12";
    const auto path_setting = [](const char* key, std::filesystem::path fallback) {
        const char* value = std::getenv(key);
        return value && *value ? std::filesystem::u8path(value) : fallback;
    };
    const auto pack = path_setting("SFR_SHADER_PACK", "out/shaders/shaders.pack");
    const auto bundled = path_setting("SFR_PIPELINE_MANIFEST", pack.parent_path() / (std::string("pipelines-") + name + ".manifest"));
    impl_->learned_manifest = path_setting("SFR_PIPELINE_MANIFEST_LOCAL",
        std::filesystem::path("pipeline-cache") / (std::string(name) + ".manifest"));
    for (const auto& path : {impl_->learned_manifest, bundled}) {
        try {
            const auto entries = load_pipeline_manifest_file(path, backend);
            size_t stale = 0;
            for (const auto& recipe : entries) {
                const auto* vs = packed_pipeline_shader(ShaderStage::vertex, recipe.vertex.hash, recipe.vertex.size);
                const auto* ps = packed_pipeline_shader(ShaderStage::pixel, recipe.pixel.hash, recipe.pixel.size);
                if (!vs || !ps || (recipe.state.pixel_spec_constants & ~ps->specialization_mask) ||
                    (recipe.pixel_link_constants & ~ps->specialization_mask)) {
                    ++stale;
                    continue;
                }
                if (impl_->recipes.size() < pipeline_manifest_max_recipes)
                    impl_->recipes.emplace(pipeline_recipe_key(recipe), recipe);
            }
            if (stale && path == impl_->learned_manifest) impl_->manifest_dirty = true;
            std::cerr << "PIPELINE_MANIFEST loaded=" << entries.size() << " stale=" << stale
                      << " path=" << path.generic_string() << '\n';
        } catch (const std::invalid_argument& error) {
            std::cerr << "PIPELINE_MANIFEST ignored=" << path.generic_string() << " reason=" << error.what() << '\n';
        }
    }
    const char* enabled = std::getenv("SFR_PIPELINE_PREWARM");
    if (enabled && *enabled == '0') {
        std::cerr << "PIPELINE_PREWARM disabled=1 recording=1\n";
        return;
    }
    if (impl_->recipes.empty()) {
        std::cerr << "PIPELINE_PREWARM total=0 recording=1\n";
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    std::atomic<size_t> completed{0};
    std::atomic<bool> done{false}, cancel{false};
    size_t skipped = 0, compiled = 0;
    std::exception_ptr failure;
    const size_t total = impl_->recipes.size();
    // Present once before any worker/driver activity, including creation of
    // the preparation screen's own blit resources.
    if (!impl_->presentation.preparation_progress(0, total))
        throw RuntimeStop("window-closed", 0, "pipeline preparation cancelled");
    std::jthread worker([&] {
        try {
            for (const auto& [key, recipe] : impl_->recipes) {
                if (cancel.load(std::memory_order_relaxed)) break;
                // Only known packaged stages: never launch a translator from
                // untrusted manifest contents. Pack corruption remains fatal.
                const auto* vs = packed_pipeline_shader(ShaderStage::vertex, recipe.vertex.hash, recipe.vertex.size);
                const auto* ps = packed_pipeline_shader(ShaderStage::pixel, recipe.pixel.hash, recipe.pixel.size);
                if (!vs || !ps || (recipe.state.pixel_spec_constants & ~ps->specialization_mask) ||
                    (recipe.pixel_link_constants & ~ps->specialization_mask)) {
                    ++skipped;
                } else {
                    const auto shader = [&](const ShaderCacheEntry& entry) -> const plume::RenderShader* {
                        auto& result = impl_->prepared_shaders[&entry];
                        if (!result) {
                            const auto bytes = entry.code(backend == 1);
                            result = impl_->graphics.device().createShader(bytes.data(), bytes.size(), "shaderMain",
                                backend == 1 ? plume::RenderShaderFormat::SPIRV : plume::RenderShaderFormat::DXIL);
                            if (!result || (backend == 1 &&
                                static_cast<const plume::VulkanShader*>(result.get())->vk == VK_NULL_HANDLE))
                                throw std::runtime_error("prepared shader creation failed");
                        }
                        return result.get();
                    };
                    NativeDraw state = recipe.state;
                    state.vertex_shader = shader(*vs);
                    state.pixel_shader = backend == 0 && ps->specialization_mask ?
                        specialized(*ps, recipe.pixel_link_constants) : shader(*ps);
                    auto pipeline = create_draw_pipeline(impl_->graphics.device(), impl_->layout.get(), state,
                                                         impl_->graphics.backend());
                    if (pipeline) {
                        impl_->recipe_pipelines.emplace(key, std::move(pipeline));
                        ++compiled;
                    } else {
                        ++skipped;
                        std::cerr << "PIPELINE_PREWARM failed=1 recipe=" << completed.load() << '\n';
                    }
                }
                completed.fetch_add(1, std::memory_order_release);
            }
        } catch (...) { failure = std::current_exception(); }
        done.store(true, std::memory_order_release);
    });
    try {
        while (!done.load(std::memory_order_acquire)) {
            if (!impl_->presentation.preparation_progress(completed.load(std::memory_order_acquire), total,
                                                          cancel.load(std::memory_order_relaxed)))
                cancel.store(true, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
    } catch (...) {
        cancel.store(true, std::memory_order_relaxed);
        worker.join();
        throw;
    }
    worker.join();
    if (cancel.load(std::memory_order_relaxed))
        throw RuntimeStop("window-closed", 0, "pipeline preparation cancelled");
    if (failure) std::rethrow_exception(failure);
    if (!impl_->presentation.preparation_progress(total, total))
        throw RuntimeStop("window-closed", 0, "pipeline preparation cancelled");
    impl_->presentation.finish_preparation();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    std::cerr << "PIPELINE_PREWARM complete=1 total=" << total << " compiled=" << compiled
              << " skipped=" << skipped << " milliseconds=" << ms << '\n';
}

NativeRenderer::NativeRenderer(NativeGraphics& graphics, NativePresentation& presentation)
    : impl_(std::make_unique<Impl>(graphics, presentation)) {
    if (const char* setting = std::getenv("SFR_CONSTANT_REUSE_TRACE"); setting && *setting == '1')
        impl_->constant_reuse_probe = std::make_unique<Impl::ConstantReuseProbe>();
    if (const char* setting = std::getenv("SFR_CONSTANT_UPLOAD_REUSE"); setting && *setting == '1')
        impl_->constant_uploads = std::make_unique<std::array<NativeConstantUpload, 2>>();
    auto& device = graphics.device();
    plume::RenderPipelineLayoutBuilder layout;
    layout.begin(false, true);
    plume::RenderDescriptorSetBuilder textures;
    textures.begin();
    textures.addTexture(0, texture_capacity);
    textures.end(true, texture_capacity);
    impl_->textures = textures.create(&device);
    for (int space = 0; space < 3; ++space) layout.addDescriptorSet(textures);
    plume::RenderDescriptorSetBuilder samplers;
    samplers.begin();
    samplers.addSampler(0, sampler_capacity);
    samplers.end(true, sampler_capacity);
    impl_->samplers = samplers.create(&device);
    layout.addDescriptorSet(samplers);
    plume::RenderDescriptorSetBuilder survey;
    survey.begin();
    survey.addReadWriteStructuredBuffer(0);
    survey.end();
    impl_->survey = survey.create(&device);
    layout.addDescriptorSet(survey);
    if (graphics.backend() == GraphicsBackend::vulkan) {
        // XenosRecomp's SPIR-V reads its constants through buffer addresses
        // in push constants: vertex, pixel and shared, and the two this
        // project adds for the skinning palette and the loop constants. Those
        // two were read out of the shared constants with a 64-bit
        // vk::RawBufferLoad, an indirection every skinned draw paid for and
        // an under-aligned load a strict driver need not perform as meant.
        layout.addPushConstant(0, 0, 5 * sizeof(uint64_t),
                               plume::RenderShaderStageFlag::VERTEX | plume::RenderShaderStageFlag::PIXEL);
    } else {
        // b0..b2 are the shader constants, b3 the skinning palette a vertex
        // shader fetches from stream 1 (vertex_palette.h) and b4 its loop
        // constants.
        for (uint32_t b = 0; b < 5; ++b) layout.addRootDescriptor(b, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER);
    }
    layout.end();
    impl_->layout = layout.create(&device);
    if (!impl_->layout || !impl_->textures || !impl_->samplers || !impl_->survey)
        unsupported(0, "native draw pipeline layout creation failed");

    impl_->survey_buffer = device.createBuffer(plume::RenderBufferDesc::DefaultBuffer(
        1024 * sizeof(uint32_t), plume::RenderBufferFlag::STORAGE | plume::RenderBufferFlag::UNORDERED_ACCESS));
    const plume::RenderBufferStructuredView survey_view(sizeof(uint32_t));
    impl_->survey->setBuffer(0, impl_->survey_buffer.get(), 0, &survey_view);

    // Null descriptors 0..2 read as zero, like Marathon's blank textures.
    for (uint32_t i = 0; i < 3; ++i) {
        plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(1, 1, 1, plume::RenderFormat::R8_UNORM);
        plume::RenderTextureViewDesc view = plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R8_UNORM);
        if (i == null_cube) {
            desc.arraySize = 6;
            desc.flags = plume::RenderTextureFlag::CUBE;
            view = plume::RenderTextureViewDesc::TextureCube(plume::RenderFormat::R8_UNORM);
        }
        view.componentMapping = plume::RenderComponentMapping(plume::RenderSwizzle::ZERO, plume::RenderSwizzle::ZERO,
                                                              plume::RenderSwizzle::ZERO, plume::RenderSwizzle::ZERO);
        auto texture = device.createTexture(desc);
        auto texture_view = texture->createTextureView(view);
        impl_->textures->setTexture(i, texture.get(), plume::RenderTextureLayout::SHADER_READ, texture_view.get());
        impl_->texture_objects.push_back(std::move(texture));
        impl_->texture_views.push_back(std::move(texture_view));
    }
    impl_->sampler_objects.push_back(device.createSampler(plume::RenderSamplerDesc{}));
    impl_->samplers->setSampler(0, impl_->sampler_objects.back().get());

    // Vertex inputs the declaration does not provide read slot 15: zeros.
    impl_->zero_buffer = device.createBuffer(plume::RenderBufferDesc::VertexBuffer(256, plume::RenderHeapType::UPLOAD));
    std::memset(impl_->zero_buffer->map(), 0, 256);
    impl_->zero_buffer->unmap();

    for (auto& upload : impl_->uploads) {
        upload.list = graphics.queue().createCommandList();
        upload.fence = device.createCommandFence();
    }

    for (int i = 0; i < 2; ++i) {
        impl_->rings[i] = device.createBuffer(plume::RenderBufferDesc::UploadBuffer(
            ring_size, plume::RenderBufferFlag::VERTEX | plume::RenderBufferFlag::CONSTANT |
                           plume::RenderBufferFlag::INDEX));
        if (!impl_->rings[i]) unsupported(0, "native upload ring creation failed");
        impl_->rings_mapped[i] = static_cast<uint8_t*>(impl_->rings[i]->map());
    }
    const char* batch_setting = std::getenv("SFR_TEXTURE_UPLOAD_BATCH");
    const char* wait_setting = std::getenv("SFR_TEXTURE_UPLOAD_WAIT");
    if ((!batch_setting || *batch_setting != '0') && (!wait_setting || *wait_setting != '1'))
        impl_->upload_batch = std::make_unique<NativeUploadBatch>(device, graphics.queue());
    auto* state = impl_.get();
    presentation.before_submit([state] { if (state->upload_batch) state->upload_batch->submit(); });
    presentation.after_flush([state](bool complete) {
        if (complete && state->upload_batch) state->upload_batch->finish();
        if (state->constant_reuse_probe) state->constant_reuse_probe->valid = false;
        if (state->constant_uploads)
            for (auto& upload : *state->constant_uploads) upload.reset();
        ++state->frame;
        ++state->texture_generation;
        state->ring_offset = 0;
        auto& free = state->free_texture_indices;
        free.insert(free.end(), state->in_flight_texture_indices.begin(), state->in_flight_texture_indices.end());
        state->in_flight_texture_indices.clear();
        if (complete) {
            free.insert(free.end(), state->released_texture_indices.begin(), state->released_texture_indices.end());
        } else {
            // The frame just submitted still reads its ring and textures; the
            // one before it has finished with the other ring.
            state->in_flight_texture_indices.swap(state->released_texture_indices);
            state->ring_index ^= 1;
        }
        state->released_texture_indices.clear();
        // Dropped vertex buffers outlive the frame in flight that may read them.
        std::erase_if(state->retired_vertex_buffers, [&](const auto& retired) { return retired.first + 2 < state->frame; });
        // Ranges not drawn for ten seconds or so are forgotten.
        if (state->frame % 600 == 0)
            std::erase_if(state->vertex_entries, [&](auto& item) {
                if (item.second.last_frame + 600 >= state->frame) return false;
                state->retire_vertices(item.second);
                return true;
            });
    });
}

NativeRenderer::~NativeRenderer() {
    // Complete recorded draws while their upload ring and textures still exist.
    try { impl_->presentation.flush(); } catch (...) {}
    impl_->presentation.clear_before_submit();
    impl_->presentation.clear_after_flush();
    impl_->save_manifest();
}
uint32_t NativeRenderer::draws() const noexcept { return impl_->draws; }

NativeRenderer::PipelineWork NativeRenderer::take_pipeline_work() noexcept {
    if (impl_->manifest_dirty && std::chrono::steady_clock::now() - impl_->manifest_saved_at > std::chrono::seconds(30))
        impl_->save_manifest();
    PipelineWork work{impl_->pipelines_created, impl_->pipeline_ms,
                            impl_->ring_flushes, impl_->textures_uploaded, impl_->texture_ms};
    if (auto* probe = impl_->constant_reuse_probe.get()) {
        work.constant_upload_bytes = probe->uploaded;
        work.constant_reusable_bytes = probe->reusable;
        probe->uploaded = probe->reusable = 0;
    }
    if (impl_->upload_batch) {
        const auto& stats = impl_->upload_batch->stats();
        work.upload_submissions = stats.submissions - impl_->batch_submissions_seen;
        work.upload_wait_ms = stats.wait_ms - impl_->batch_wait_ms_seen;
        work.upload_peak_bytes = stats.peak_bytes;
        work.upload_budget_drains = stats.budget_drains;
        impl_->batch_submissions_seen = stats.submissions;
        impl_->batch_wait_ms_seen = stats.wait_ms;
    }
    work.constant_saved_bytes = impl_->constant_saved_bytes;
    impl_->constant_saved_bytes = 0;
    impl_->pipelines_created = 0;
    impl_->pipeline_ms = 0;
    impl_->ring_flushes = 0;
    impl_->textures_uploaded = 0;
    impl_->texture_ms = 0;
    return work;
}

NativeRenderer::CachedVertices NativeRenderer::vertex_cache(GuestMemory& memory, uint32_t physical,
                                                            uint64_t bytes, uint64_t host_bytes, uint64_t layout) {
    constexpr uint64_t budget = 512ull << 20;  // host-visible memory for cached vertices
    memory.enable_write_epochs();
    std::array<uint32_t, 3> views{};
    uint32_t view_count = 0;
    for (uint64_t candidate : {uint64_t(physical) + 0xE0000000u - 0x1000u, uint64_t(physical) | 0xA0000000u,
                               uint64_t(physical) | 0xC0000000u})
        if (candidate + bytes <= 0x100000000ull && memory.readable(candidate, bytes)) views[view_count++] = uint32_t(candidate);
    if (!view_count || !bytes) return {};
    auto& entry = impl_->vertex_entries[Impl::VertexKey{views[0], bytes, layout}];
    const uint32_t now = memory.write_epoch();
    entry.last_frame = impl_->frame;
    const auto written = [&] {
        for (uint32_t i = 0; i < entry.view_count; ++i)
            if (memory.written_since(entry.views[i], bytes, entry.epoch)) return true;
        return false;
    };
    if (!entry.view_count) {
        // New: watch every view, and see whether it stays unwritten.
        entry.views = views;
        entry.view_count = view_count;
        for (uint32_t i = 0; i < view_count; ++i) memory.watch_writes(views[i], bytes);
        entry.epoch = now;
        return {};
    }
    if (written()) {
        impl_->retire_vertices(entry);
        entry.epoch = now;
        return {};
    }
    if (entry.buffer) return {entry.buffer.get(), {}};
    if (entry.epoch >= now || impl_->cached_vertex_bytes + host_bytes > budget) return {};
    // Unwritten for a whole frame: keep it. Stores from here on (including
    // later in this frame) show as written at the next lookup.
    entry.buffer = impl_->graphics.device().createBuffer(
        plume::RenderBufferDesc::UploadBuffer(host_bytes, plume::RenderBufferFlag::VERTEX));
    if (!entry.buffer) return {};
    auto* mapped = static_cast<uint8_t*>(entry.buffer->map());
    if (!mapped) { entry.buffer.reset(); return {}; }
    impl_->cached_vertex_bytes += host_bytes;
    entry.size = host_bytes;
    entry.epoch = now;
    return {entry.buffer.get(), {mapped, size_t(host_bytes)}};
}
uint64_t NativeRenderer::texture_generation() const noexcept { return impl_->texture_generation; }

std::span<uint8_t> NativeRenderer::vertex_space(uint64_t bytes, uint64_t index_bytes) {
    // The most a draw can add after its vertices (see draw()).
    const uint64_t worst = align(bytes, 256) + 4096 + 4096 + 512 + palette_bytes + 512 + index_bytes;
    impl_->reserved = nullptr;
    if (!bytes || worst > ring_size) return {};
    if (impl_->ring_offset + worst > ring_size) { ++impl_->ring_flushes; impl_->presentation.flush(); }  // resets the ring
    uint8_t* const at = impl_->rings_mapped[impl_->ring_index] + impl_->ring_offset;
    impl_->reserved = at;
    impl_->reserved_offset = impl_->ring_offset;
    impl_->reserved_ring = impl_->ring_index;
    return {at, size_t(bytes)};
}

const plume::RenderShader* NativeRenderer::specialized(const ShaderCacheEntry& entry, uint32_t spec_constants) {
    spec_constants &= entry.specialization_mask;
    const uint64_t key = (uint64_t(reinterpret_cast<uintptr_t>(entry.dxil.data())) << 8) ^ spec_constants;
    auto& shader = impl_->linked[key];
    if (!shader) {
#ifdef _WIN32
        if (!impl_->dxc) impl_->dxc = std::make_unique<DxcLinker>();
        const auto bytes = impl_->dxc->link(entry.dxil, spec_constants);
        shader = impl_->graphics.device().createShader(bytes.data(), bytes.size(), "shaderMain",
                                                       plume::RenderShaderFormat::DXIL);
#endif
        if (!shader) unsupported(spec_constants, "linked pixel shader creation failed");
    }
    return shader.get();
}

// One flat white texture for formats without a native layout (investigation).
uint32_t NativeRenderer::placeholder_texture() {
    if (impl_->placeholder) return *impl_->placeholder;
    auto& device = impl_->graphics.device();
    constexpr auto format = plume::RenderFormat::R8G8B8A8_UNORM;
    auto texture = device.createTexture(plume::RenderTextureDesc::Texture2D(1, 1, 1, format));
    if (!texture) unsupported(0, "native placeholder texture creation failed");
    auto staging = device.createBuffer(plume::RenderBufferDesc::UploadBuffer(256));
    auto* mapped = static_cast<uint8_t*>(staging->map());
    std::memset(mapped, 0xFF, 4);
    staging->unmap();
    auto& slot0 = impl_->uploads[0];
    auto& list = *slot0.list;
    if (slot0.in_flight) {
        impl_->graphics.queue().waitForCommandFence(slot0.fence.get());
        slot0.in_flight = false;
        slot0.staging.clear();
    }
    list.begin();
    list.barriers(plume::RenderBarrierStage::COPY,
                  plume::RenderTextureBarrier(texture.get(), plume::RenderTextureLayout::COPY_DEST));
    list.copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(texture.get()),
        plume::RenderTextureCopyLocation::PlacedFootprint(staging.get(), format, 1, 1, 1, 64));
    list.barriers(plume::RenderBarrierStage::GRAPHICS,
                  plume::RenderTextureBarrier(texture.get(), plume::RenderTextureLayout::SHADER_READ));
    list.end();
    impl_->graphics.queue().executeCommandLists(&list, slot0.fence.get());
    impl_->graphics.queue().waitForCommandFence(slot0.fence.get());
    uint32_t index;
    if (!impl_->free_texture_indices.empty()) {
        index = impl_->free_texture_indices.back();
        impl_->free_texture_indices.pop_back();
    } else {
        index = first_texture + impl_->next_texture_index++;
    }
    auto view = texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(format));
    impl_->textures->setTexture(index, texture.get(), plume::RenderTextureLayout::SHADER_READ, view.get());
    const size_t slot = index - first_texture;
    if (impl_->texture_objects.size() <= slot) {
        impl_->texture_objects.resize(slot + 1);
        impl_->texture_views.resize(slot + 1);
    }
    impl_->texture_objects[slot] = std::move(texture);
    impl_->texture_views[slot] = std::move(view);
    impl_->placeholder = index;
    return index;
}

uint32_t NativeRenderer::adopt_resolved_target(uint32_t physical) {
    auto& device = impl_->graphics.device();
    auto& presentation = impl_->presentation;
    const uint32_t width = presentation.render_width(), height = presentation.render_height();
    constexpr auto format = plume::RenderFormat::B8G8R8A8_UNORM;
    uint32_t index;
    if (auto found = impl_->resolved_targets.find(physical); found != impl_->resolved_targets.end()) {
        index = found->second;
    } else {
        auto texture = device.createTexture(plume::RenderTextureDesc::Texture2D(width, height, 1, format));
        if (!texture) unsupported(physical, "native resolve target creation failed");
        if (!impl_->free_texture_indices.empty()) {
            index = impl_->free_texture_indices.back();
            impl_->free_texture_indices.pop_back();
        } else {
            index = first_texture + impl_->next_texture_index++;
        }
        if (index >= texture_capacity) unsupported(index, "native texture descriptor capacity exhausted");
        auto view = texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(format));
        impl_->textures->setTexture(index, texture.get(), plume::RenderTextureLayout::SHADER_READ, view.get());
        const size_t slot = index - first_texture;
        if (impl_->texture_objects.size() <= slot) {
            impl_->texture_objects.resize(slot + 1);
            impl_->texture_views.resize(slot + 1);
        }
        impl_->texture_objects[slot] = std::move(texture);
        impl_->texture_views[slot] = std::move(view);
        impl_->resolved_targets.emplace(physical, index);
        impl_->resolved_texture_indices.set(index);
        ++impl_->texture_generation;
    }
    // Recorded in the frame's own command list, after the draws it copies and
    // before the ones that sample it: submitting and waiting here instead
    // would cost a GPU round trip for each of a frame's resolves.
    auto* target = impl_->texture_objects[index - first_texture].get();
    presentation.record_async([color = &presentation.color(), target](plume::RenderCommandList& list, uint64_t) {
        const std::array<plume::RenderTextureBarrier, 2> before{
            plume::RenderTextureBarrier(color, plume::RenderTextureLayout::COPY_SOURCE),
            plume::RenderTextureBarrier(target, plume::RenderTextureLayout::COPY_DEST)};
        list.barriers(plume::RenderBarrierStage::COPY, before.data(), uint32_t(before.size()));
        list.copyTexture(target, color);
        const std::array<plume::RenderTextureBarrier, 2> after{
            plume::RenderTextureBarrier(target, plume::RenderTextureLayout::SHADER_READ),
            plume::RenderTextureBarrier(color, plume::RenderTextureLayout::COLOR_WRITE)};
        list.barriers(plume::RenderBarrierStage::GRAPHICS, after.data(), uint32_t(after.size()));
    });
    return index;
}

uint32_t NativeRenderer::texture(GuestMemory& memory, const FetchWords& words) {
    // Timed as a whole: the lookup, the content hash that decides whether the
    // guest changed it, and the upload when it did.
    const auto texture_start = std::chrono::steady_clock::now();
    struct TextureTimer {
        Impl& impl; std::chrono::steady_clock::time_point start;
        ~TextureTimer() {
            impl.texture_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
        }
    } texture_timer{*impl_, texture_start};
    const auto fetch = decode_texture_fetch(words);
    if (!fetch.base_address) return null_2d;
    // What the title resolved out of the framebuffer is served from the copy
    // taken then, whatever the guest memory at that address holds.
    if (auto resolved = impl_->resolved_targets.find(fetch.base_address); resolved != impl_->resolved_targets.end()) {
        static std::set<uint32_t> sampled;
        if (sampled.insert(fetch.base_address).second)
            std::cerr << "NATIVE_RESOLVED_SAMPLED base=0x" << std::hex << fetch.base_address << std::dec
                      << " descriptor=" << resolved->second << '\n';
        return resolved->second;
    }
    // SFR_TEXTURE_SURVEY=N reports the first N distinct texture addresses a
    // draw samples, to compare them with what the title resolved.
    static const uint32_t survey = [] { const char* t = std::getenv("SFR_TEXTURE_SURVEY"); return t ? uint32_t(std::strtoul(t, nullptr, 10)) : 0u; }();
    if (survey) {
        static std::set<uint32_t> seen;
        if (seen.size() < survey && seen.insert(fetch.base_address).second)
            std::cerr << "NATIVE_TEXTURE_SURVEY base=0x" << std::hex << fetch.base_address << std::dec
                      << " size=" << fetch.width << "x" << fetch.height << " format=" << fetch.format << '\n';
    }
    const std::array<uint32_t, 4> key{words[0] & 0x80000000u | ((words[0] >> 22) & 0x1FF), words[1] & ~0x800u, words[2], words[5]};
    // Every mapped view of a physical range (writes may use any of them).
    const auto views = [&](uint32_t physical, uint64_t size, const auto& visit) {
        for (uint64_t candidate : {uint64_t(physical) + 0xE0000000u - 0x1000u, uint64_t(physical) | 0xA0000000u,
                                   uint64_t(physical) | 0xC0000000u}) {
            if (candidate + size > 0x100000000ull) continue;
            if (!memory.readable(candidate, size)) continue;
            visit(candidate);
        }
    };
    if (auto found = impl_->texture_indices.find(key); found != impl_->texture_indices.end()) {
        // The CPU may rewrite a texture in place (movie frames) without a lock:
        // watched pages report it and the texture is uploaded again.
        auto& range = impl_->texture_ranges.at(found->second);
        // Checked once a frame, not once a draw: the scan below walks the
        // watched-page bits of the whole texture in each of three views, and a
        // race frame binds the same textures across hundreds of draws
        // (docs/performance.md). A rewrite lands one frame later, which is how
        // the content check beside it already behaves.
        if (range.seen_frame == impl_->frame) return found->second;
        range.seen_frame = impl_->frame;
        bool written = false;
        views(uint32_t(range.begin), range.end - range.begin, [&](uint64_t view) {
            written |= memory.take_written(view, range.end - range.begin);
        });
        if (!written && range.dynamic && range.checked_frame != impl_->frame) {
            range.checked_frame = impl_->frame;
            // Content check for textures the CPU rewrites (e.g. movie planes).
            const uint64_t size = range.end - range.begin;
            const uint32_t source = guest_view(memory, uint32_t(range.begin), size);
            written = content_hash(memory.base() + source, size) != range.hash;
        }
        static const bool no_cache = std::getenv("SFR_TEXTURE_NO_CACHE") != nullptr;
        if (!written && !(fetch.format == 2 && no_cache)) return found->second;
        invalidate(uint32_t(range.begin), uint32_t(range.end - range.begin));
    }
    auto layout = linear_texture_layout(fetch);
    if (!layout && depth_texture_placeholder) {
        // Investigation mode: a format without a native layout is served as a
        // flat white texture, so that a race keeps rendering with the wrong
        // content instead of stopping at the first such texture.
        static std::set<uint32_t> reported_formats;
        if (reported_formats.insert(fetch.format).second)
            std::cerr << "NATIVE_TEXTURE_PLACEHOLDER format=" << fetch.format << " tiled=" << fetch.tiled
                      << " size=" << fetch.width << 'x' << fetch.height << '\n';
        return placeholder_texture();
    }
    if (!layout)
        unsupported(fetch.format, "unsupported texture (format " + std::to_string(fetch.format) + ", tiled " +
                                  std::to_string(fetch.tiled) + ", dimension " + std::to_string(int(fetch.dimension)) + ")");
    const uint64_t guest_size = uint64_t(layout->row_bytes) * layout->guest_rows;
    const uint32_t source = guest_view(memory, fetch.base_address, guest_size);
    std::vector<uint8_t> bytes(guest_size);
    memory.check(source, guest_size);
    std::memcpy(bytes.data(), memory.base() + source, guest_size);
    const uint64_t hash = content_hash(bytes.data(), bytes.size());
    swap_texture_bytes(bytes, fetch.endian);
    if (layout->tiled || layout->base_x_blocks || layout->base_y_blocks)
        bytes = untile_texture(bytes, *layout);
    if (fetch.format == 15) decode_rgba4(bytes, *layout);
    if (!block_compression_supported(impl_->graphics)) decode_block_compression(bytes, *layout);
    static const bool stats = std::getenv("SFR_TEXTURE_STATS") != nullptr;
    if (stats && fetch.format == 2) {
        uint64_t sum = 0, nonzero = 0;
        for (const uint8_t b : bytes) { sum += b; nonzero += b != 0; }
        std::cerr << "TEXTURE_STATS physical=0x" << std::hex << fetch.base_address << std::dec << " mean="
                  << double(sum) / double(bytes.size()) << " nonzero=" << nonzero << '\n';
    }

    auto& device = impl_->graphics.device();
    const uint32_t blocks = (layout->width + layout->block_width - 1) / layout->block_width;
    const uint32_t copy_row = uint32_t(align(uint64_t(blocks) * layout->block_bytes, 256));
    auto staging = device.createBuffer(plume::RenderBufferDesc::UploadBuffer(uint64_t(copy_row) * layout->rows));
    auto* mapped = static_cast<uint8_t*>(staging->map());
    for (uint32_t row = 0; row < layout->rows; ++row)
        std::memcpy(mapped + uint64_t(row) * copy_row, bytes.data() + uint64_t(row) * layout->row_bytes,
                    std::min<uint64_t>(layout->row_bytes, uint64_t(blocks) * layout->block_bytes));
    staging->unmap();
    auto texture = device.createTexture(plume::RenderTextureDesc::Texture2D(layout->width, layout->height, 1, layout->format));
    if (!texture) unsupported(fetch.format, "native texture creation failed");
    // Released indices are recycled only after their consuming frame finishes.
    uint32_t index;
    if (!impl_->free_texture_indices.empty()) {
        index = impl_->free_texture_indices.back();
        impl_->free_texture_indices.pop_back();
    } else {
        index = first_texture + impl_->next_texture_index++;
    }
    if (index >= texture_capacity) unsupported(index, "native texture descriptor capacity exhausted");
    auto view = texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(layout->format));
    impl_->textures->setTexture(index, texture.get(), plume::RenderTextureLayout::SHADER_READ, view.get());
    const size_t slot = index - first_texture;
    if (impl_->texture_objects.size() <= slot) {
        impl_->texture_objects.resize(slot + 1);
        impl_->texture_views.resize(slot + 1);
    }
    impl_->texture_objects[slot] = std::move(texture);
    impl_->texture_views[slot] = std::move(view);
    // Retain the destination before recording a copy. A later descriptor or
    // metadata failure must not leave teardown submitting a dangling texture.
    auto* upload_texture = impl_->texture_objects[slot].get();
    if (impl_->upload_batch) {
        impl_->upload_batch->record(std::move(staging), uint64_t(copy_row) * layout->rows,
            [&](plume::RenderCommandList& list, plume::RenderBuffer& source_buffer) {
                list.barriers(plume::RenderBarrierStage::COPY,
                    plume::RenderTextureBarrier(upload_texture, plume::RenderTextureLayout::COPY_DEST));
                list.copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(upload_texture),
                    plume::RenderTextureCopyLocation::PlacedFootprint(&source_buffer, layout->format,
                        layout->width, layout->height, 1, copy_row / layout->block_bytes * layout->block_width));
                list.barriers(plume::RenderBarrierStage::GRAPHICS,
                    plume::RenderTextureBarrier(upload_texture, plume::RenderTextureLayout::SHADER_READ));
            });
    } else {
        // SFR_TEXTURE_UPLOAD_WAIT=1 restores the old behaviour, waiting for each
        // upload before the draw that asked for it: a way to tell whether a hang
        // is this path's doing.
        static const bool wait_for_uploads = [] {
            const char* text = std::getenv("SFR_TEXTURE_UPLOAD_WAIT");
            return text && *text == '1';
        }();
        const uint32_t upload_slot = wait_for_uploads ? 0 : impl_->upload_slot;
        impl_->upload_slot = (upload_slot + 1) % Impl::upload_lists;
        auto& upload = impl_->uploads[upload_slot];
        if (upload.in_flight) {
            impl_->graphics.queue().waitForCommandFence(upload.fence.get());
            upload.in_flight = false;
            upload.staging.clear();  // the GPU has finished with them
        }
        auto& list = *upload.list;
        list.begin();
        list.barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(upload_texture, plume::RenderTextureLayout::COPY_DEST));
        list.copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(upload_texture),
            plume::RenderTextureCopyLocation::PlacedFootprint(staging.get(), layout->format, layout->width, layout->height, 1,
                                                              copy_row / layout->block_bytes * layout->block_width));
        list.barriers(plume::RenderBarrierStage::GRAPHICS,
                      plume::RenderTextureBarrier(upload_texture, plume::RenderTextureLayout::SHADER_READ));
        list.end();
        // Submitted without waiting: a queue runs its submissions in order, so
        // this copy is done before the frame that samples the texture, which is
        // submitted after it. Waiting here cost a GPU round trip per texture -
        // about 1.3 ms each, and nearly half of the slowest race frames. The
        // staging buffer has to outlive the copy, so it is kept as long as a
        // retired vertex buffer is.
        impl_->graphics.queue().executeCommandLists(&list, upload.fence.get());
        upload.in_flight = true;
        upload.staging.push_back(std::move(staging));
        if (wait_for_uploads) {
            impl_->graphics.queue().waitForCommandFence(upload.fence.get());
            upload.in_flight = false;
            upload.staging.clear();
        }
    }


    impl_->texture_indices.emplace(key, index);
    ++impl_->texture_generation;
    const uint64_t physical = fetch.base_address;
    impl_->texture_ranges[index] = {key, physical, physical + guest_size, hash, impl_->dynamic_ranges.contains(physical)};
    views(fetch.base_address, guest_size, [&](uint64_t view) {
        memory.watch_writes(view, guest_size);
        memory.take_written(view, guest_size);
    });
    ++impl_->textures_uploaded;
    static uint64_t uploads = 0;
    if (uploads++ < 256)
    std::cerr << "NATIVE_TEXTURE_UPLOAD index=" << index << " format=" << fetch.format << " size=" << layout->width
              << 'x' << layout->height << " endian=" << fetch.endian << " source=0x" << std::hex << source
              << std::dec << " bytes=" << guest_size << '\n';
    return index;
}

void NativeRenderer::invalidate(uint32_t physical, uint32_t size) {
    const uint64_t begin = physical, end = uint64_t(physical) + size;
    // Whole-allocation frees also retire resolves whose destination begins
    // inside that allocation. Keep their objects/descriptors alive for draws
    // already recorded or submitted, just like ordinary texture versions.
    // Only the destination base is known here, not its guest byte extent.
    for (auto it = impl_->resolved_targets.lower_bound(physical);
         it != impl_->resolved_targets.end() && uint64_t(it->first) < end;) {
        impl_->resolved_texture_indices.reset(it->second);
        impl_->released_texture_indices.push_back(it->second);
        ++impl_->texture_generation;
        it = impl_->resolved_targets.erase(it);
    }
    for (auto it = impl_->texture_ranges.begin(); it != impl_->texture_ranges.end();) {
        if (it->second.begin < end && begin < it->second.end) {
            impl_->dynamic_ranges.insert(it->second.begin);
            impl_->texture_indices.erase(it->second.key);
            ++impl_->texture_generation;
            impl_->released_texture_indices.push_back(it->first);
            it = impl_->texture_ranges.erase(it);
        } else {
            ++it;
        }
    }
}

uint32_t NativeRenderer::guest_address(GuestMemory& memory, uint32_t physical, uint64_t size) {
    return guest_view(memory, physical, size);
}

uint32_t NativeRenderer::sampler(const FetchWords& words) {
    const std::array<uint32_t, 2> key{words[0] & 0x0007FC00u, words[3] & 0x01F80000u};
    if (auto found = impl_->sampler_indices.find(key); found != impl_->sampler_indices.end()) return found->second;
    const uint32_t index = uint32_t(impl_->sampler_objects.size());
    if (index >= sampler_capacity) unsupported(index, "native sampler descriptor capacity exhausted");
    impl_->sampler_objects.push_back(impl_->graphics.device().createSampler(fetch_sampler(words)));
    impl_->samplers->setSampler(index, impl_->sampler_objects.back().get());
    impl_->sampler_indices.emplace(key, index);
    return index;
}

void NativeRenderer::draw(const NativeDraw& draw) {
    auto& device = impl_->graphics.device();
    static const bool bulk_key = [] {
        const char* value = std::getenv("SFR_PIPELINE_KEY_BULK");
        return !value || *value != '0';
    }();
    static const bool verify_key = [] {
        const char* value = std::getenv("SFR_PIPELINE_KEY_VERIFY");
        const bool verify = value && *value != '0';
        std::cerr << "NATIVE_PIPELINE_KEY bulk=" << bulk_key << " verify=" << verify << '\n';
        return verify;
    }();
    static thread_local std::vector<uint8_t> key;
    if (bulk_key) native_pipeline_key_bulk(draw, key);
    else native_pipeline_key_legacy(draw, key);
    if (verify_key) {
        static thread_local std::vector<uint8_t> reference;
        if (bulk_key) native_pipeline_key_legacy(draw, reference);
        else native_pipeline_key_bulk(draw, reference);
        if (key != reference) unsupported(0, "pipeline key serializer mismatch");
        static thread_local uint64_t verified = 0;
        if (++verified == 1 || verified % 100000 == 0)
            std::cerr << "NATIVE_PIPELINE_KEY verified=" << verified << '\n';
    }
    auto& pipeline = impl_->pipelines[key];  // copies the key only when inserting
    const std::array<plume::RenderInputSlot, 2> slots{plume::RenderInputSlot(0, draw.stride),
                                                       plume::RenderInputSlot(zero_slot, 0)};
    std::optional<PipelineRecipe> recipe;
    std::vector<uint8_t> recipe_key;
    if (!pipeline && !impl_->learned_manifest.empty() && draw.vertex_entry && draw.pixel_entry) {
        try {
            recipe.emplace();
            recipe->vertex = pipeline_shader_id(draw.vertex_entry->source);
            recipe->pixel = pipeline_shader_id(draw.pixel_entry->source);
            recipe->pixel_link_constants = draw.pixel_link_constants;
            recipe->state = draw;
            recipe_key = pipeline_recipe_key(*recipe);
            if (const auto found = impl_->recipe_pipelines.find(recipe_key); found != impl_->recipe_pipelines.end()) {
                pipeline = found->second;
                ++impl_->prewarm_hits;
                std::cerr << "PIPELINE_PREWARM hit=" << impl_->prewarm_hits << '\n';
            }
        } catch (const std::invalid_argument& error) {
            std::cerr << "PIPELINE_MANIFEST unrecordable=" << error.what() << '\n';
            recipe.reset();
        }
    }
    if (!pipeline) {
        const auto pipeline_start = std::chrono::steady_clock::now();
        pipeline = create_draw_pipeline(device, impl_->layout.get(), draw, impl_->graphics.backend());
        if (!pipeline) unsupported(0, "native graphics pipeline creation failed");
        ++impl_->pipelines_created;
        impl_->pipeline_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - pipeline_start).count();
        if (recipe) {
            impl_->recipe_pipelines.emplace(recipe_key, pipeline);
            if (impl_->recipes.size() < pipeline_manifest_max_recipes) {
                // Round-trip removes per-draw pointers, spans and constants;
                // only immutable owned pipeline state is retained.
                const auto encoded = encode_pipeline_manifest(std::span(&*recipe, 1), uint32_t(impl_->graphics.backend()));
                auto clean = decode_pipeline_manifest(encoded, uint32_t(impl_->graphics.backend()));
                impl_->recipes.insert_or_assign(recipe_key, std::move(clean.front()));
                impl_->manifest_dirty = true;
            }
        }
    }

    // Per-draw upload: vertices, then the three constant buffers (256-aligned).
    // Per-draw data lives in a persistent upload ring that is recycled after
    // the frame's command list completes.
    const uint64_t vertex_bytes = draw.vertex_buffer ? 0 : draw.vertices.size();  // in the ring
    const uint64_t index_bytes = draw.indices.size() * sizeof(uint32_t);
    const uint64_t vs_rel = align(vertex_bytes, 256), ps_rel = vs_rel + 4096,
                   shared_rel = ps_rel + 4096, palette_rel = shared_rel + 512,
                   loop_rel = palette_rel + (draw.palette.empty() ? 0 : palette_bytes),
                   index_rel = align(loop_rel + 256, 256), total = index_rel + index_bytes;
    if (total > ring_size) unsupported(uint32_t(total), "draw data exceeds the upload ring");
    // Vertices already written in place by the caller (vertex_space), which
    // also made room for the rest of this draw.
    const bool in_place = vertex_bytes && draw.vertices.data() == impl_->reserved &&
        impl_->reserved_ring == impl_->ring_index && impl_->reserved_offset == impl_->ring_offset;
    impl_->reserved = nullptr;
    if (!in_place && impl_->ring_offset + total > ring_size) { ++impl_->ring_flushes; impl_->presentation.flush(); }
    const uint64_t base_offset = impl_->ring_offset;
    impl_->ring_offset = align(base_offset + total, 256);
    auto* upload = impl_->rings[impl_->ring_index].get();
    uint8_t* mapped = impl_->rings_mapped[impl_->ring_index] + base_offset;
    if (!in_place) std::memcpy(mapped, draw.vertices.data(), vertex_bytes);
    if (auto* probe = impl_->constant_reuse_probe.get()) {
        const std::array<const std::array<uint32_t, 1024>*, 2> stages{
            &draw.vertex_constants, &draw.pixel_constants};
        for (size_t stage = 0; stage < stages.size(); ++stage) {
            const auto& current = *stages[stage];
            probe->uploaded += sizeof(current);
            if (probe->valid && std::memcmp(probe->previous[stage].data(), current.data(), sizeof(current)) == 0)
                probe->reusable += sizeof(current);
            else
                probe->previous[stage] = current;
        }
        probe->valid = true;
    }
    uint64_t vs_offset = base_offset + vs_rel, ps_offset = base_offset + ps_rel;
    if (auto* constants = impl_->constant_uploads.get()) {
        // A hit references an immutable slot from this same ring lifetime.
        // Keep the allocation layout unchanged, including the unused slots.
        const auto vs = (*constants)[0].upload(draw.vertex_constants, vs_offset, mapped + vs_rel);
        const auto ps = (*constants)[1].upload(draw.pixel_constants, ps_offset, mapped + ps_rel);
        vs_offset = vs.offset;
        ps_offset = ps.offset;
        impl_->constant_saved_bytes += (uint64_t(vs.reused) + uint64_t(ps.reused)) * 4096;
    } else {
        std::memcpy(mapped + vs_rel, draw.vertex_constants.data(), 4096);
        std::memcpy(mapped + ps_rel, draw.pixel_constants.data(), 4096);
    }
    std::memcpy(mapped + shared_rel, &draw.shared, sizeof(draw.shared));
    if (impl_->presentation.width() != impl_->presentation.render_width() ||
        impl_->presentation.height() != impl_->presentation.render_height()) {
        auto shared = draw.shared;
        shared.resolved_texture_scale[0] = float(impl_->presentation.width()) / impl_->presentation.render_width();
        shared.resolved_texture_scale[1] = float(impl_->presentation.height()) / impl_->presentation.render_height();
        for (auto& descriptor : shared.texture_2d)
            if (descriptor < texture_capacity && impl_->resolved_texture_indices.test(descriptor)) descriptor |= 0x80000000u;
        std::memcpy(mapped + shared_rel, &shared, sizeof(shared));
    }
    const bool vulkan = impl_->graphics.backend() == GraphicsBackend::vulkan;
    const uint64_t upload_address = vulkan ? upload->getDeviceAddress() : 0;
    const uint64_t ring_address = upload_address + base_offset;
    // Only the entries the palette holds are written. The rest of the
    // allocation keeps whatever an earlier draw left, which a clamped index
    // may read but never reaches past the ring; zeroing sixteen kilobytes for
    // every draw cost more than the draw itself.
    if (!draw.palette.empty())
        std::memcpy(mapped + palette_rel, draw.palette.data(),
                    (std::min)(size_t(palette_bytes), draw.palette.size()));
    std::memcpy(mapped + loop_rel, draw.loop_constants.data(), sizeof(draw.loop_constants));
    if (index_bytes) std::memcpy(mapped + index_rel, draw.indices.data(), index_bytes);
    const uint64_t shared_offset = base_offset + shared_rel;

    // Recorded by the render thread (NativePresentation::record_async), so
    // everything it reads is copied here, none of it drawn from `draw` or the
    // frame's locals once this call returns. impl's bound_* fields are the
    // render thread's alone.
    const bool palette_bound = !draw.palette.empty();
    const bool stencil_enabled = draw.stencil_enabled;
    const uint8_t stencil_reference = draw.stencil_reference;
    const uint32_t stride = draw.stride, vertex_count = draw.vertex_count;
    const uint32_t vertex_view_bytes = uint32_t(draw.vertex_buffer ? uint64_t(draw.vertex_count) * draw.stride
                                                                   : draw.vertices.size());
    const plume::RenderBuffer* const vertex_buffer = draw.vertex_buffer;
    const uint32_t index_count = uint32_t(draw.indices.size());
    const int32_t base_vertex_location = draw.base_vertex_location;
    impl_->presentation.record_async([impl = impl_.get(), pipeline = pipeline.get(), palette_bound, stencil_enabled,
                                      stencil_reference, stride, vertex_count, vertex_view_bytes, vertex_buffer,
                                      index_count, base_vertex_location, index_bytes, vulkan, upload_address,
                                      ring_address, upload, base_offset, vs_offset, ps_offset, shared_rel,
                                      palette_rel, loop_rel, index_rel, shared_offset](plume::RenderCommandList& list,
                                                                                   uint64_t generation) {
        const std::array<plume::RenderInputSlot, 2> slots{plume::RenderInputSlot(0, stride),
                                                           plume::RenderInputSlot(zero_slot, 0)};
        const bool fresh = impl->bound_generation != generation;
        if (fresh) {
            impl->bound_generation = generation;
            list.setGraphicsPipelineLayout(impl->layout.get());
            for (uint32_t space = 0; space < 3; ++space) list.setGraphicsDescriptorSet(impl->textures.get(), space);
            list.setGraphicsDescriptorSet(impl->samplers.get(), 3);
            list.setGraphicsDescriptorSet(impl->survey.get(), 4);
            const plume::RenderVertexBufferView zeros(plume::RenderBufferReference(impl->zero_buffer.get(), 0), 256);
            list.setVertexBuffers(zero_slot, &zeros, 1, &slots[1]);
            impl->bound_pipeline = nullptr;
        }
        if (impl->bound_pipeline != pipeline) {
            list.setPipeline(pipeline);
            impl->bound_pipeline = pipeline;
        }
#ifdef _WIN32
        // Plume's D3D12 backend sets the pipeline's stencil reference at each
        // draw but never stores RenderGraphicsPipelineDesc::stencilReference,
        // so it is always 0: a HUD gauge's mask wrote 0 and its fill (drawn
        // where the stencil equals 1) covered the whole gauge. Set it here.
        // (Vulkan keeps it in the pipeline.)
        if (stencil_enabled && !vulkan)
            static_cast<plume::D3D12CommandList&>(list).d3d->OMSetStencilRef(stencil_reference);
#endif
        if (vulkan) {
            const uint64_t addresses[5] = {upload_address + vs_offset, upload_address + ps_offset, ring_address + shared_rel,
                                           palette_bound ? ring_address + palette_rel : 0,
                                           ring_address + loop_rel};
            list.setGraphicsPushConstants(0, addresses, 0, sizeof(addresses));
        } else {
            list.setGraphicsRootDescriptor(plume::RenderBufferReference(upload, vs_offset), 0);
            list.setGraphicsRootDescriptor(plume::RenderBufferReference(upload, ps_offset), 1);
            list.setGraphicsRootDescriptor(plume::RenderBufferReference(upload, shared_offset), 2);
            if (palette_bound)
                list.setGraphicsRootDescriptor(plume::RenderBufferReference(upload, base_offset + palette_rel), 3);
            list.setGraphicsRootDescriptor(plume::RenderBufferReference(upload, base_offset + loop_rel), 4);
        }
        const plume::RenderVertexBufferView vertices(
            vertex_buffer ? plume::RenderBufferReference(const_cast<plume::RenderBuffer*>(vertex_buffer), 0)
                          : plume::RenderBufferReference(upload, base_offset),
            vertex_view_bytes);
        list.setVertexBuffers(0, &vertices, 1, &slots[0]);
        // An indexed draw selects its vertices from the uploaded block; the
        // base location puts that block back where the stream holds it.
        if (index_bytes) {
            const plume::RenderIndexBufferView view(plume::RenderBufferReference(upload, base_offset + index_rel),
                                                    uint32_t(index_bytes), plume::RenderFormat::R32_UINT);
            list.setIndexBuffer(&view);
            list.drawIndexedInstanced(index_count, 1, 0, base_vertex_location, 0);
        } else {
            list.drawInstanced(vertex_count, 1, 0, 0);
        }
    });
    ++impl_->draws;
}
}
