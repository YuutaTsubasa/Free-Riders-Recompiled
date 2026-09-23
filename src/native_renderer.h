#pragma once
#include "native_blend_control.h"
#include "native_render_state.h"
#include "shader_cache.h"
#include "texture_fetch.h"
#include "plume_render_interface_types.h"
#include <cstddef>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace plume { struct RenderShader; struct RenderBuffer; }
namespace sfr {
class GuestMemory;
class NativeGraphics;
class NativePresentation;

// Shared constants read by XenosRecomp shaders (cbuffer b2, space4): texture
// descriptor indices per sampler register for 2D, 2D array and cube views,
// sampler indices, then the DEFINE_SHARED_CONSTANTS block at c16.
struct SharedConstants {
    uint32_t texture_2d[16], texture_2d_array[16], texture_cube[16], sampler[16];
    uint32_t booleans, swapped_texcoords, swapped_normals, swapped_binormals;
    uint32_t swapped_tangents, swapped_blend_weights;
    float half_pixel_offset[2];
    float clip_plane[4];
    uint32_t clip_plane_enabled;
    float alpha_threshold;
    uint32_t survey_index, rendering_index;
    // Extension read only by runtime-translated vertex shaders (c20.xy): with
    // the viewport transform disabled (PA_CL_VTE_CNTL scale/offset clear) the
    // shader outputs pixel coordinates, mapped to clip space by
    // xy * scale + (-w, w). Zero leaves positions unchanged.
    float screen_space_scale[2];
    uint64_t reserved;
};
static_assert(sizeof(SharedConstants) == 336);
static_assert(offsetof(SharedConstants, screen_space_scale) == 320);

struct NativeDraw {
    plume::RenderPrimitiveTopology topology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    uint32_t vertex_count = 0;
    uint32_t stride = 0;
    // Both spans point at the caller's scratch and are read during draw():
    // a draw that allocates its own vectors puts a race frame in the C
    // runtime's heap lock, which every guest thread shares
    // (docs/performance.md).
    std::span<const uint8_t> vertices;  // little-endian words
    // Or a buffer that already holds them (vertex_cache), vertex_count vertices
    // of stride bytes; vertices is then not read.
    const plume::RenderBuffer* vertex_buffer = nullptr;
    // An indexed draw uploads the contiguous block of the stream its indices
    // reach and selects vertices from it, instead of gathering one vertex at a
    // time: the gather is by far a race frame's largest cost. Empty for an
    // unindexed draw.
    std::span<const uint32_t> indices;
    // Added to every index, so the block can start anywhere in the stream.
    int32_t base_vertex_location = 0;
    std::vector<plume::RenderInputElement> elements;
    // Skinning palette (stream 1) for a vertex shader that fetches one, in
    // little-endian float4 rows; bound as the b3 constant buffer. Scratch of
    // the caller's, like vertices and indices.
    std::span<const uint8_t> palette;
    const plume::RenderShader* vertex_shader = nullptr;
    const plume::RenderShader* pixel_shader = nullptr;
    // Vulkan: the pixel shader's specialization constant (constant_id 0),
    // set in its pipeline; D3D12 links it into the shader instead.
    uint32_t pixel_spec_constants = 0;
    std::array<uint32_t, 1024> vertex_constants{};  // 256 float4
    std::array<uint32_t, 1024> pixel_constants{};
    // Vertex loop constants i0..i15, unpacked to int4 (see loop_constants.h).
    std::array<int32_t, 64> loop_constants{};
    SharedConstants shared{};
    NativeBlendControl blend;
    uint8_t write_mask = 0xF;
    bool depth_enabled = false, depth_write = false;
    plume::RenderComparisonFunction depth_function = plume::RenderComparisonFunction::LESS_EQUAL;
    // Stencil test (the title masks HUD gauges with it), from RB_DEPTHCONTROL
    // and RB_STENCILREFMASK (native_stencil).
    bool stencil_enabled = false;
    uint8_t stencil_reference = 0, stencil_read_mask = 0xFF, stencil_write_mask = 0xFF;
    plume::RenderStencilFaceDesc stencil_front, stencil_back;
    plume::RenderCullMode cull = plume::RenderCullMode::NONE;
};

class NativeRenderer {
public:
    NativeRenderer(NativeGraphics& graphics, NativePresentation& presentation);
    ~NativeRenderer();
    NativeRenderer(const NativeRenderer&) = delete;
    NativeRenderer& operator=(const NativeRenderer&) = delete;

    // Links a specialization-library pixel shader with the given constants.
    const plume::RenderShader* specialized(const ShaderCacheEntry& entry, uint32_t spec_constants);
    // Descriptor index of the texture described by a fetch constant, uploading
    // it from guest memory the first time. Returns a null descriptor when unbound.
    uint32_t texture(GuestMemory& memory, const FetchWords& words);
    uint32_t sampler(const FetchWords& words);
    // Guest virtual address at which the GPU physical range is mapped.
    static uint32_t guest_address(GuestMemory& memory, uint32_t physical, uint64_t size);
    // Registers a copy of the current framebuffer as the texture the title
    // resolved to this destination address, so later draws that sample the
    // address read what was rendered. Returns the descriptor index.
    uint32_t adopt_resolved_target(uint32_t physical);
    // Flat white texture used for formats without a native layout while
    // SFR_ALLOW_RENDER_TARGETS is set.
    uint32_t placeholder_texture();
    // Forgets cached textures whose guest physical data overlaps the range;
    // the next draw that uses one uploads it again.
    void invalidate(uint32_t physical, uint32_t size);
    void draw(const NativeDraw& draw);
    // Room in the upload ring for the next draw's vertices (and, after them,
    // the rest of that draw's data with index_bytes of indices), so the
    // caller writes the vertices there instead of into scratch that draw()
    // copies again. Write only: the ring is write-combined. draw() takes the
    // vertices in place when NativeDraw::vertices is this span; anything
    // else recorded in between just makes it copy them. Empty when a draw
    // that large does not fit the ring.
    std::span<uint8_t> vertex_space(uint64_t bytes, uint64_t index_bytes);
    // Vertices that stay the same from frame to frame (a level's geometry)
    // are kept in buffers of their own instead of being swapped into the
    // ring every frame. For bytes of guest vertex data at a physical address,
    // read in a given layout: the buffer holding them in host form (hit), or
    // one to fill now (fill non-empty, write only), or neither (use the ring:
    // the range is new, or was written since). Stores to any mapped view of
    // the range drop its buffer (GuestMemory write epochs); a range is kept
    // once it has gone a frame without one.
    struct CachedVertices {
        const plume::RenderBuffer* buffer = nullptr;
        std::span<uint8_t> fill;
    };
    // bytes: the guest data watched; host_bytes: the buffer (layouts that
    // repack elements widen the vertices).
    CachedVertices vertex_cache(GuestMemory& memory, uint32_t physical, uint64_t bytes, uint64_t host_bytes,
                                uint64_t layout);
    uint32_t draws() const noexcept;
    // Pipelines created since the last call, and the milliseconds spent
    // creating them: a draw that meets a state combination for the first time
    // compiles its pipeline there and then, which is what a frame that takes
    // twice as long as its neighbours is usually doing.
    struct PipelineWork {
        uint32_t created; double milliseconds;
        // Ring flushes: the upload ring filled mid-frame, which submits what
        // is recorded and waits for the GPU. Texture work: the uploads a
        // draw makes before it can bind what it fetches.
        uint32_t ring_flushes, textures; double texture_milliseconds;
    };
    PipelineWork take_pipeline_work() noexcept;
    // Unchanged while texture() and sampler() would answer the same fetch
    // words the same way, so a caller may keep their answers until it moves.
    uint64_t texture_generation() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
