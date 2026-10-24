#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "spirv_bounded_descriptors.h"

namespace plume {
enum class RenderFormat;
struct RenderCommandQueue;
struct RenderDevice;
struct RenderShader;
}

namespace sfr {
// Direct3D 12 by default; SFR_GRAPHICS=vulkan selects Vulkan (the only
// backend elsewhere than Windows); a machine where no adapter can make
// Plume's D3D12 device falls back to Vulkan. Chosen once per process: the
// shader cache prepares bytecode for this backend.
enum class GraphicsBackend { d3d12, vulkan };
GraphicsBackend selected_graphics_backend();
const char* graphics_backend_name(GraphicsBackend backend);
// The driver version a backend reports, the way its vendor prints it: D3D12's
// user-mode driver version is four 16-bit parts (31.0.21912.14); Vulkan packs
// major.minor.patch in 10, 10 and 12 bits, except NVIDIA, whose major takes
// 10 bits and minor 8 (576.52.0). 0 (not reported) is "unknown".
std::string graphics_driver_version(GraphicsBackend backend, uint32_t vendor_id, uint64_t packed);

class NativeGraphics {
public:
    NativeGraphics();
    ~NativeGraphics();

    NativeGraphics(const NativeGraphics&) = delete;
    NativeGraphics& operator=(const NativeGraphics&) = delete;

    void initialize();
    [[nodiscard]] bool initialized() const noexcept;
    plume::RenderDevice& device();
    plume::RenderCommandQueue& queue();
    [[nodiscard]] GraphicsBackend backend() const noexcept;
    // Vulkan without descriptor indexing (Issue #134): the renderer binds each
    // draw's textures and samplers in small sets of its own instead of one
    // array of thousands, and its shaders are rewritten for that layout
    // (spirv_bounded_descriptors.h). SFR_TEST_NO_DESCRIPTOR_INDEXING=1 makes
    // the device without the features to try it on a GPU that has them.
    [[nodiscard]] bool bounded_descriptors() const noexcept;
    // The bounded layout's texture bindings: the full one, or the compact one
    // for a driver with fewer textures a stage.
    [[nodiscard]] BoundedLengths bounded_lengths() const noexcept;
    // The format of every depth target and of the pipelines drawing into
    // them: D32_FLOAT_S8_UINT, or D24_UNORM_S8_UINT on a Vulkan driver
    // without it (an old Mali's). SFR_TEST_DEPTH_D24S8=1 takes D24S8 on any
    // Vulkan device.
    [[nodiscard]] plume::RenderFormat depth_format() const noexcept;
    // Vulkan with fewer vertex attributes than the twenty input locations
    // every draw declares (an old Mali's 16, Issue #169): each vertex shader
    // takes its inputs at compacted locations and its pipelines declare only
    // those (spirv_vertex_inputs.h). SFR_TEST_VERTEX_ATTRIBUTES=<n> takes n
    // as the device's attributes.
    [[nodiscard]] bool compacts_vertex_inputs() const noexcept;
    // Whether shader_code changes SPIR-V on this device: the bounded layout,
    // vertex shaders without their clip distance (spirv_clip_distance.h:
    // Mali, or a device without shaderClipDistance), with compacted inputs,
    // or SPIR-V 1.3 for a Vulkan 1.1 device (spirv_version.h), and on Apple
    // without XenosRecomp's dispatch loops (spirv_dispatch_loop.h).
    [[nodiscard]] bool rewrites_shaders() const noexcept;
    // The shader's code as this device takes it: SPIR-V without its clip
    // distance, with compacted vertex inputs, in the bounded layout and as
    // SPIR-V 1.3 as the device needs, otherwise as it is. A vertex shader
    // whose inputs were compacted gets their original locations in
    // vertex_inputs (for VertexInputUse::note once it is made).
    [[nodiscard]] std::vector<uint8_t> shader_code(std::span<const uint8_t> code,
                                                   std::optional<std::vector<uint32_t>>* vertex_inputs = nullptr) const;
    // Whether shader_code takes XenosRecomp's dispatch loops out
    // (spirv_dispatch_loop.h; SFR_TEST_DISPATCH_LOOP).
    [[nodiscard]] bool removes_dispatch_loops() const noexcept;
    // A driver can refuse a pipeline with a shader whose dispatch loop came
    // out: an Adreno 740 failed to link 17 of the game's 455 ("Failed to
    // link shaders.", VK_ERROR_UNKNOWN), and the game stopped at the first
    // such draw. Every shader made from shader_code's code is made known
    // here with its code as it came (which must outlive this: the shader
    // cache's entries do); with_dispatch_loop then makes the same shader with
    // its loop kept, once, for a pipeline to be made again with. Null for a
    // shader that kept its loop anyway (nothing to fall back to).
    void note_shader(const plume::RenderShader* shader, std::span<const uint8_t> code);
    [[nodiscard]] const plume::RenderShader* with_dispatch_loop(const plume::RenderShader* shader);
    // Linux: the SDL window Plume's Vulkan instance was made for (hidden
    // until presentation shows it). Null on Windows.
    [[nodiscard]] void* host_window() const noexcept;
    void wait_idle(const std::function<bool()>& cancelled = {});

private:
    [[nodiscard]] std::vector<uint8_t> rewritten_code(std::span<const uint8_t> code,
                                                      std::optional<std::vector<uint32_t>>* vertex_inputs,
                                                      bool keep_dispatch_loop) const;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
