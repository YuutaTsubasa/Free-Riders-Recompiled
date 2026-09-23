#include "native_renderer.h"
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
#include <atomic>
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
    std::set<uint64_t> dynamic_ranges;  // physical starts of rewritten textures
    std::map<uint32_t, TextureRange> texture_ranges;  // by descriptor index
    // Destination address of a resolve to its descriptor index (the copy of
    // the framebuffer the title samples afterwards).
    std::map<uint32_t, uint32_t> resolved_targets;
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
    std::unordered_map<std::vector<uint8_t>, std::unique_ptr<plume::RenderPipeline>, KeyHash> pipelines;
    std::unique_ptr<plume::RenderCommandList> upload_list;
    std::unique_ptr<plume::RenderCommandFence> upload_fence;
    std::vector<std::unique_ptr<plume::RenderBuffer>> retired;
#ifdef _WIN32
    std::unique_ptr<DxcLinker> dxc;
#endif
    uint32_t draws = 0;
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

NativeRenderer::NativeRenderer(NativeGraphics& graphics, NativePresentation& presentation)
    : impl_(std::make_unique<Impl>(graphics, presentation)) {
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
        // XenosRecomp's SPIR-V reads its constants through three buffer
        // addresses in push constants (vertex, pixel, shared); the palette
        // and loop constants are addressed from the shared constants.
        layout.addPushConstant(0, 0, 3 * sizeof(uint64_t),
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

    impl_->upload_list = graphics.queue().createCommandList();
    impl_->upload_fence = device.createCommandFence();

    for (int i = 0; i < 2; ++i) {
        impl_->rings[i] = device.createBuffer(plume::RenderBufferDesc::UploadBuffer(
            ring_size, plume::RenderBufferFlag::VERTEX | plume::RenderBufferFlag::CONSTANT |
                           plume::RenderBufferFlag::INDEX));
        if (!impl_->rings[i]) unsupported(0, "native upload ring creation failed");
        impl_->rings_mapped[i] = static_cast<uint8_t*>(impl_->rings[i]->map());
    }
    auto* state = impl_.get();
    presentation.after_flush([state](bool complete) {
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
    impl_->presentation.clear_after_flush();
}
uint32_t NativeRenderer::draws() const noexcept { return impl_->draws; }

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
    if (impl_->ring_offset + worst > ring_size) impl_->presentation.flush();  // resets the ring
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
    auto& list = *impl_->upload_list;
    list.begin();
    list.barriers(plume::RenderBarrierStage::COPY,
                  plume::RenderTextureBarrier(texture.get(), plume::RenderTextureLayout::COPY_DEST));
    list.copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(texture.get()),
        plume::RenderTextureCopyLocation::PlacedFootprint(staging.get(), format, 1, 1, 1, 64));
    list.barriers(plume::RenderBarrierStage::GRAPHICS,
                  plume::RenderTextureBarrier(texture.get(), plume::RenderTextureLayout::SHADER_READ));
    list.end();
    impl_->graphics.queue().executeCommandLists(&list, impl_->upload_fence.get());
    impl_->graphics.queue().waitForCommandFence(impl_->upload_fence.get());
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
    const uint32_t width = presentation.width(), height = presentation.height();
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
        ++impl_->texture_generation;
    }
    // Recorded in the frame's own command list, after the draws it copies and
    // before the ones that sample it: submitting and waiting here instead
    // would cost a GPU round trip for each of a frame's resolves.
    auto* target = impl_->texture_objects[index - first_texture].get();
    presentation.record([&](plume::RenderCommandList& list) {
        const std::array<plume::RenderTextureBarrier, 2> before{
            plume::RenderTextureBarrier(&presentation.color(), plume::RenderTextureLayout::COPY_SOURCE),
            plume::RenderTextureBarrier(target, plume::RenderTextureLayout::COPY_DEST)};
        list.barriers(plume::RenderBarrierStage::COPY, before.data(), uint32_t(before.size()));
        list.copyTexture(target, &presentation.color());
        const std::array<plume::RenderTextureBarrier, 2> after{
            plume::RenderTextureBarrier(target, plume::RenderTextureLayout::SHADER_READ),
            plume::RenderTextureBarrier(&presentation.color(), plume::RenderTextureLayout::COLOR_WRITE)};
        list.barriers(plume::RenderBarrierStage::GRAPHICS, after.data(), uint32_t(after.size()));
    });
    return index;
}

uint32_t NativeRenderer::texture(GuestMemory& memory, const FetchWords& words) {
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
    if (layout->tiled) bytes = untile_texture(bytes, *layout);
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
    auto& list = *impl_->upload_list;
    list.begin();
    list.barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(texture.get(), plume::RenderTextureLayout::COPY_DEST));
    list.copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(texture.get()),
        plume::RenderTextureCopyLocation::PlacedFootprint(staging.get(), layout->format, layout->width, layout->height, 1,
                                                          copy_row / layout->block_bytes * layout->block_width));
    list.barriers(plume::RenderBarrierStage::GRAPHICS,
                  plume::RenderTextureBarrier(texture.get(), plume::RenderTextureLayout::SHADER_READ));
    list.end();
    impl_->graphics.queue().executeCommandLists(&list, impl_->upload_fence.get());
    impl_->graphics.queue().waitForCommandFence(impl_->upload_fence.get());

    // Every draw waits for its command list, so a released slot is unused.
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
    impl_->texture_indices.emplace(key, index);
    ++impl_->texture_generation;
    const uint64_t physical = fetch.base_address;
    impl_->texture_ranges[index] = {key, physical, physical + guest_size, hash, impl_->dynamic_ranges.contains(physical)};
    views(fetch.base_address, guest_size, [&](uint64_t view) {
        memory.watch_writes(view, guest_size);
        memory.take_written(view, guest_size);
    });
    static uint64_t uploads = 0;
    if (uploads++ < 256)
    std::cerr << "NATIVE_TEXTURE_UPLOAD index=" << index << " format=" << fetch.format << " size=" << layout->width
              << 'x' << layout->height << " endian=" << fetch.endian << " source=0x" << std::hex << source
              << std::dec << " bytes=" << guest_size << '\n';
    return index;
}

void NativeRenderer::invalidate(uint32_t physical, uint32_t size) {
    const uint64_t begin = physical, end = uint64_t(physical) + size;
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
    // Pipeline key: shaders, inputs, stride, topology and fixed-function state.
    // Built in a buffer that keeps its capacity: a key allocated for every
    // draw showed up in a race frame's profile beside the map lookup itself.
    static thread_local std::vector<uint8_t> key;
    key.clear();
    const auto put = [&](const void* data, size_t size) {
        key.insert(key.end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    };
    put(&draw.vertex_shader, sizeof(void*));
    put(&draw.pixel_shader, sizeof(void*));
    put(&draw.pixel_spec_constants, 4);
    for (const auto& e : draw.elements) {
        put(&e.semanticIndex, 4); put(&e.format, sizeof(e.format)); put(&e.slotIndex, 4); put(&e.alignedByteOffset, 4);
        // The name by its address: every one comes from declaration_semantic's
        // static table, so the pointer identifies it without a strlen a draw.
        put(&e.semanticName, sizeof(e.semanticName));
    }
    put(&draw.stride, 4); put(&draw.topology, sizeof(draw.topology)); put(&draw.blend, sizeof(draw.blend));
    put(&draw.write_mask, 1); put(&draw.depth_enabled, 1); put(&draw.depth_write, 1);
    put(&draw.depth_function, sizeof(draw.depth_function)); put(&draw.cull, sizeof(draw.cull));
    put(&draw.stencil_enabled, 1);
    if (draw.stencil_enabled) {
        put(&draw.stencil_reference, 1); put(&draw.stencil_read_mask, 1); put(&draw.stencil_write_mask, 1);
        put(&draw.stencil_front, sizeof(draw.stencil_front)); put(&draw.stencil_back, sizeof(draw.stencil_back));
    }
    auto& pipeline = impl_->pipelines[key];  // copies the key only when inserting
    const std::array<plume::RenderInputSlot, 2> slots{plume::RenderInputSlot(0, draw.stride),
                                                       plume::RenderInputSlot(zero_slot, 0)};
    if (!pipeline) {
        plume::RenderGraphicsPipelineDesc desc;
        desc.pipelineLayout = impl_->layout.get();
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
        pipeline = device.createGraphicsPipeline(desc);
        if (!pipeline) unsupported(0, "native graphics pipeline creation failed");
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
    if (!in_place && impl_->ring_offset + total > ring_size) impl_->presentation.flush();  // resets the ring
    const uint64_t base_offset = impl_->ring_offset;
    impl_->ring_offset = align(base_offset + total, 256);
    auto* upload = impl_->rings[impl_->ring_index].get();
    uint8_t* mapped = impl_->rings_mapped[impl_->ring_index] + base_offset;
    if (!in_place) std::memcpy(mapped, draw.vertices.data(), vertex_bytes);
    std::memcpy(mapped + vs_rel, draw.vertex_constants.data(), 4096);
    std::memcpy(mapped + ps_rel, draw.pixel_constants.data(), 4096);
    std::memcpy(mapped + shared_rel, &draw.shared, sizeof(draw.shared));
    const bool vulkan = impl_->graphics.backend() == GraphicsBackend::vulkan;
    const uint64_t ring_address = vulkan ? upload->getDeviceAddress() + base_offset : 0;
    if (vulkan) {
        auto* shared = reinterpret_cast<SharedConstants*>(mapped + shared_rel);
        shared->palette_address = draw.palette.empty() ? 0 : ring_address + palette_rel;
        shared->loop_address = ring_address + loop_rel;
    }
    // Only the entries the palette holds are written. The rest of the
    // allocation keeps whatever an earlier draw left, which a clamped index
    // may read but never reaches past the ring; zeroing sixteen kilobytes for
    // every draw cost more than the draw itself.
    if (!draw.palette.empty())
        std::memcpy(mapped + palette_rel, draw.palette.data(),
                    (std::min)(size_t(palette_bytes), draw.palette.size()));
    std::memcpy(mapped + loop_rel, draw.loop_constants.data(), sizeof(draw.loop_constants));
    if (index_bytes) std::memcpy(mapped + index_rel, draw.indices.data(), index_bytes);
    const uint64_t vs_offset = base_offset + vs_rel, ps_offset = base_offset + ps_rel,
                   shared_offset = base_offset + shared_rel;

    impl_->presentation.record([&](plume::RenderCommandList& list) {
        const bool fresh = impl_->bound_generation != impl_->presentation.list_generation();
        if (fresh) {
            impl_->bound_generation = impl_->presentation.list_generation();
            list.setGraphicsPipelineLayout(impl_->layout.get());
            for (uint32_t space = 0; space < 3; ++space) list.setGraphicsDescriptorSet(impl_->textures.get(), space);
            list.setGraphicsDescriptorSet(impl_->samplers.get(), 3);
            list.setGraphicsDescriptorSet(impl_->survey.get(), 4);
            const plume::RenderVertexBufferView zeros(plume::RenderBufferReference(impl_->zero_buffer.get(), 0), 256);
            list.setVertexBuffers(zero_slot, &zeros, 1, &slots[1]);
            impl_->bound_pipeline = nullptr;
        }
        if (impl_->bound_pipeline != pipeline.get()) {
            list.setPipeline(pipeline.get());
            impl_->bound_pipeline = pipeline.get();
        }
#ifdef _WIN32
        // Plume's D3D12 backend sets the pipeline's stencil reference at each
        // draw but never stores RenderGraphicsPipelineDesc::stencilReference,
        // so it is always 0: a HUD gauge's mask wrote 0 and its fill (drawn
        // where the stencil equals 1) covered the whole gauge. Set it here.
        // (Vulkan keeps it in the pipeline.)
        if (draw.stencil_enabled && !vulkan)
            static_cast<plume::D3D12CommandList&>(list).d3d->OMSetStencilRef(draw.stencil_reference);
#endif
        if (vulkan) {
            const uint64_t addresses[3] = {ring_address + vs_rel, ring_address + ps_rel, ring_address + shared_rel};
            list.setGraphicsPushConstants(0, addresses, 0, sizeof(addresses));
        } else {
            list.setGraphicsRootDescriptor(plume::RenderBufferReference(upload, vs_offset), 0);
            list.setGraphicsRootDescriptor(plume::RenderBufferReference(upload, ps_offset), 1);
            list.setGraphicsRootDescriptor(plume::RenderBufferReference(upload, shared_offset), 2);
            if (!draw.palette.empty())
                list.setGraphicsRootDescriptor(plume::RenderBufferReference(upload, base_offset + palette_rel), 3);
            list.setGraphicsRootDescriptor(plume::RenderBufferReference(upload, base_offset + loop_rel), 4);
        }
        const plume::RenderVertexBufferView vertices(
            draw.vertex_buffer ? plume::RenderBufferReference(const_cast<plume::RenderBuffer*>(draw.vertex_buffer), 0)
                               : plume::RenderBufferReference(upload, base_offset),
            uint32_t(draw.vertex_buffer ? uint64_t(draw.vertex_count) * draw.stride : draw.vertices.size()));
        list.setVertexBuffers(0, &vertices, 1, &slots[0]);
        // An indexed draw selects its vertices from the uploaded block; the
        // base location puts that block back where the stream holds it.
        if (index_bytes) {
            const plume::RenderIndexBufferView view(plume::RenderBufferReference(upload, base_offset + index_rel),
                                                    uint32_t(index_bytes), plume::RenderFormat::R32_UINT);
            list.setIndexBuffer(&view);
            list.drawIndexedInstanced(uint32_t(draw.indices.size()), 1, 0, draw.base_vertex_location, 0);
        } else {
            list.drawInstanced(draw.vertex_count, 1, 0, 0);
        }
    });
    ++impl_->draws;
}
}
