#pragma once
#include "native_blend_control.h"
#include "native_render_state.h"
#include "native_formats.h"
#include "shader_cache.h"
#include "texture_fetch.h"
#include "guest_constant_state.h"
#include "plume_render_interface_types.h"
#include <cstddef>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace plume { struct RenderShader; struct RenderBuffer; }
namespace sfr {
struct GuestBufferSnapshot;
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
    // Logical/physical texture-size ratio for resolved framebuffer samples.
    // Only descriptors marked by the renderer consume it; assets stay native.
    float resolved_texture_scale[2];
};
static_assert(sizeof(SharedConstants) == 336);
static_assert(offsetof(SharedConstants, screen_space_scale) == 320);
static_assert(offsetof(SharedConstants, resolved_texture_scale) == 328);

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
    // Replace the complete binding when reusing a NativeDraw: a slice offset
    // must never carry into the next legacy-cache or upload-ring draw.
    struct VertexBufferBinding {
        const plume::RenderBuffer* buffer = nullptr;
        uint64_t offset = 0;
    } vertex_buffer;
    // Converted dynamic ring slice. Uploaded lazily inside draw(), after any
    // capacity flush, then reused for this upload-ring lifetime.
    std::shared_ptr<const GuestBufferSnapshot> vertex_snapshot;
    uint64_t vertex_snapshot_offset = 0;
    // An indexed draw uploads the contiguous block of the stream its indices
    // reach and selects vertices from it, instead of gathering one vertex at a
    // time: the gather is by far a race frame's largest cost. Empty for an
    // unindexed draw.
    std::span<const uint32_t> indices;
    // Or a buffer that already holds index_count of them (vertex_cache with
    // index set): indices is then not read and nothing is copied for them.
    const plume::RenderBuffer* index_buffer = nullptr;
    uint32_t index_count = 0;
    // Added to every index, so the block can start anywhere in the stream.
    int32_t base_vertex_location = 0;
    std::vector<plume::RenderInputElement> elements;
    // Borrowed only until draw() returns. The decoded layout cache gives
    // each version a unique identity, so stable layouts need no per-draw copy.
    std::span<const plume::RenderInputElement> element_view;
    uint64_t input_layout_identity = 0;
    std::span<const plume::RenderInputElement> input_elements() const {
        return input_layout_identity ? element_view : std::span<const plume::RenderInputElement>(elements);
    }
    void own_input_elements() {
        if (input_layout_identity) elements.assign(element_view.begin(), element_view.end());
        element_view = {};
        input_layout_identity = 0;
    }
    // Skinning palette (stream 1) for a vertex shader that fetches one, in
    // little-endian float4 rows; bound as the b3 constant buffer. Scratch of
    // the caller's, like vertices and indices.
    std::span<const uint8_t> palette;
    const plume::RenderShader* vertex_shader = nullptr;
    const plume::RenderShader* pixel_shader = nullptr;
    // Source identity for portable pipeline recipes; null for non-game draws.
    const ShaderCacheEntry* vertex_entry = nullptr;
    const ShaderCacheEntry* pixel_entry = nullptr;
    uint32_t pixel_link_constants = 0;
    // Vulkan: the pixel shader's specialization constant (constant_id 0),
    // set in its pipeline; D3D12 links it into the shader instead.
    uint32_t pixel_spec_constants = 0;
    // Default zero constants for host-generated draws. Keeping views here
    // avoids initializing/copying 8 KiB of unused arrays for every game draw
    // and pipeline recipe. Callers own nonzero values until draw() returns.
    static inline constexpr GuestConstantState::Values zero_constants{};
    std::span<const uint32_t, 1024> vertex_constants{zero_constants};
    std::span<const uint32_t, 1024> pixel_constants{zero_constants};
    const GuestConstantState* vertex_constant_state = nullptr;
    const GuestConstantState* pixel_constant_state = nullptr;
    // The game's constants as the device holds them (4 KiB each, big-endian,
    // already checked), in place of the two host views above: the renderer swaps
    // them straight into the upload ring. A copy into the arrays, a swap there
    // and a copy into the ring read and wrote each draw's 8 KiB three times,
    // which a 6 MB L3 felt (Issue #65). Empty: the arrays hold them.
    // With versioned owners, nonempty sources select direct conversion only
    // on an upload miss. Spans must remain valid until draw() returns; queued
    // work retains immutable GPU offsets, never these guest pointers.
    std::span<const uint8_t> vertex_constants_source, pixel_constants_source;
    // Vertex loop constants i0..i15, unpacked to int4 (see loop_constants.h).
    std::array<int32_t, 64> loop_constants{};
    const GuestShortConstantState* short_constant_state = nullptr;
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
    // depth: the resolve's source is the depth surface (a shadow map, the
    // scene's depth), kept as a sampled depth texture. Out of one of the
    // title's own surfaces the resolve is aliased (Marathon Recompiled's
    // deferred resolve): the surface itself is sampled, and copied out only
    // before it is drawn to again (NativePresentation::mark_target_read).
    uint32_t adopt_resolved_target(uint32_t physical, bool depth = false);
    // Copies out what is still resolved from a surface about to be drawn to.
    void materialize_aliases(const void* surface);
    // Flat white texture used for formats without a native layout while
    // SFR_ALLOW_RENDER_TARGETS is set.
    uint32_t placeholder_texture();
    // Forgets cached textures whose guest physical data overlaps the range;
    // the next draw that uses one uploads it again. Resolved targets are
    // retired when their destination base lies in the range (their guest
    // extent is not tracked), including on whole-allocation frees.
    void invalidate(uint32_t physical, uint32_t size);
    void draw(const NativeDraw& draw);
    // Called once at initial device setup, before the title's first frame.
    // Compiles known recipes while the presentation thread remains responsive.
    void prepare_pipelines();
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
        uint64_t offset = 0;
    };
    // source: the actual checked guest bytes that the caller will convert,
    // within the physical range's E/A/C view. A source-view change retires the
    // old allocation even when its original view has not been written.
    // bytes: the guest data watched; host_bytes: the buffer (layouts that
    // repack elements widen the vertices).
    // index: the buffer holds 32-bit indices (an index buffer) instead.
    CachedVertices vertex_cache(GuestMemory& memory, uint32_t physical, const uint8_t* source,
                                uint64_t bytes, uint64_t host_bytes, uint64_t layout, bool index = false);
    struct CachedIndices {
        const plume::RenderBuffer* buffer = nullptr;
        IndexScan scan;
    };
    // Same guarded slice cache as vertex_cache, with R32 data and its exact
    // scan published together. source is checked guest memory, not a length;
    // count/width determine both extents. Restart draws retain their fallback.
    CachedIndices index_cache(GuestMemory& memory, uint32_t physical, const uint8_t* source,
                              uint32_t count, bool wide, uint32_t base);
    // The raw 32-bit layout converted at Unlock, uploaded once for all slices.
    // Retains old GPU allocations through completion of queued frames.
    const plume::RenderBuffer* managed_vertex_buffer(const std::shared_ptr<const GuestBufferSnapshot>& snapshot);
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
        // Opt-in probe of requested bytes and exact matches; the probe itself
        // does not skip writes. Reuse savings are reported independently.
        uint64_t constant_upload_bytes = 0, constant_reusable_bytes = 0;
        uint64_t upload_submissions = 0, upload_peak_bytes = 0, upload_budget_drains = 0;
        double upload_wait_ms = 0;
        uint64_t constant_saved_bytes = 0; // Actual skipped writes when reuse is enabled.
        uint64_t constant_written_bytes = 0; // Actual VS/PS ring writes in all modes, without the probe.
        uint64_t direct_constant_read_bytes = 0; // Guest reads on versioned direct-upload misses.
        uint64_t loop_constant_written_bytes = 0, loop_constant_saved_bytes = 0;
        // Resolves copied this frame: each ends the render pass, which a tiling
        // GPU pays for with a store and a reload of the whole color and depth.
        uint32_t resolve_copies = 0;
        uint64_t draw_ring_bytes = 0; // Bytes reserved by draws, including alignment.
        // Miss-only work: adjacent immutable vertex bindings skip map lookups.
        uint64_t managed_vertex_lookups = 0, managed_vertex_allocations = 0;
        uint64_t transient_vertex_lookups = 0;
        uint64_t index_cache_allocations = 0, index_cache_decodes = 0;
        // False means texture_milliseconds is unavailable, not zero cost.
        bool texture_timers = false;
    };
    PipelineWork take_pipeline_work() noexcept;
    // Unchanged while texture() and sampler() would answer the same fetch
    // words the same way, so a caller may keep their answers until it moves.
    uint64_t texture_generation() const noexcept;

    // Named outside for the resolve helpers in native_renderer.cpp.
    struct Impl;
private:
    struct IndexFill;
    CachedVertices vertex_cache_impl(GuestMemory& memory, uint32_t physical, const uint8_t* source,
                                     uint64_t bytes, uint64_t host_bytes, uint64_t layout,
                                     bool index, IndexFill* indices);
    std::unique_ptr<Impl> impl_;
};
}
