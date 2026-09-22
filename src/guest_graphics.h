#pragma once

#include <cstdint>
#include <memory>
#include "shader_cache.h"
#include "texture_fetch.h"

namespace sfr {
class GuestMemory;
class NativeGraphics;
class NativePresentation;
class NativeShaders;
class NativeRenderer;
struct NativeBlendControl;
enum class RenderState : uint32_t;
struct NativeRenderState;
enum class BlendRequest : uint32_t { enable = 0x3C, source = 0x48, destination = 0x4C, operation = 0x50 };
struct BlendRequestUpdate { uint32_t requested, flags, effective; bool effective_updated; };
enum class SamplerFilter : uint32_t { magnification = 0x10, minification = 0x14 };
struct SamplerFilterState {
    uint32_t word3, word4;
    uint8_t magnification, minification, mip, anisotropy;
    uint8_t volume_magnification, volume_minification;
    bool magnification_walk, minification_walk;
    bool operator==(const SamplerFilterState&) const = default;
};

class GuestGraphics {
public:
    static constexpr uint32_t device_address = 0x71600000;
    static constexpr uint32_t device_size = 0x5F00;
    // SFR_ALLOW_RENDER_TARGETS=1 (investigation): a race binds its own colour
    // and depth surfaces for the scene. The native backend has no offscreen
    // targets, so with this set those surfaces are treated as the native
    // framebuffer: the scene is drawn into the back buffer and whatever the
    // title resolves out of its surface is not what it rendered.
    static bool foreign_render_targets;
    static constexpr uint32_t color_handle = 0x71700000;
    static constexpr uint32_t depth_handle = 0x71701000;
    static constexpr uint32_t presentation_handle = 0x71702000;
    // Original packet writers store Xenos PM4 words through device+48 (write
    // pointer, pre-incremented) and call the space function past device+56.
    // Natively those packets are discarded; the margin bounds the words one
    // original writer may store between checks, and overruns hit unmapped memory.
    static constexpr uint32_t command_scratch = 0x71710000;
    static constexpr uint32_t command_scratch_size = 0x10000;
    static constexpr uint32_t command_scratch_margin = 0x4000;

    GuestGraphics(GuestMemory&, NativeGraphics&);
    ~GuestGraphics();
    GuestGraphics(const GuestGraphics&) = delete;
    GuestGraphics& operator=(const GuestGraphics&) = delete;

    uint32_t create_device(uint32_t adapter, uint32_t mode, uint32_t reserved,
                           uint32_t flags, uint32_t parameters, uint32_t output);
    bool clear(uint32_t device, uint32_t rectangle_count, uint32_t rectangles,
               uint32_t flags, uint32_t argb, float depth, uint32_t stencil);
    bool set_viewport(uint32_t device, uint32_t descriptor);
    void set_scissor(uint32_t device, uint32_t rectangle);
    void set_scissor_enabled(uint32_t device, uint32_t enabled);
    void set_blend_control(uint32_t device, uint32_t target, uint32_t packed);
    BlendRequestUpdate set_blend_request(uint32_t device, BlendRequest request, uint32_t value);
    SamplerFilterState set_sampler_filter(uint32_t device, uint32_t slot, SamplerFilter filter, uint32_t value);
    SamplerFilterState sampler_filter_state(uint32_t slot) const;
    const NativeBlendControl& blend_control(uint32_t target) const;
    void set_render_state(uint32_t device, RenderState state, uint32_t value);
    void set_primitive_restart(uint32_t device, uint32_t value);
    const NativeRenderState& render_state() const;
    bool depth_enabled() const;
    void set_texture(uint32_t device, uint32_t slot, uint32_t texture, uint64_t dirty_mask);
    uint32_t texture_binding(uint32_t slot) const;
    // The device's current fetch constant for a slot, as the GPU would read it.
    FetchWords texture_fetch(uint32_t slot) const;
    uint32_t create_shader(ShaderStage stage, uint32_t container);
    void attach_shader(uint32_t handle, uint32_t object);
    const NativeShaders& shaders() const;
    // Original 0x824E65A0: resolve RT0 into the front buffer, then swap it to the
    // display. Supported only for the native default back/front buffers.
    void present_front_buffer(uint32_t device);
    // Native draws, created with the first draw.
    NativeRenderer& renderer();
    // Discards the packets written since the last reset; returns the new write pointer.
    uint32_t discard_commands(uint32_t device, uint32_t& discarded_bytes);
    bool owns_object(uint32_t address) const noexcept;
    bool created() const noexcept;
    NativePresentation& presentation();
private:
    struct Impl;
    GuestMemory& memory_;
    NativeGraphics& graphics_;
    std::unique_ptr<Impl> impl_;
};

extern GuestGraphics* active_guest_graphics;
}
