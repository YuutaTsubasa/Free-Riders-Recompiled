#pragma once
#include "shader_cache.h"
#include <memory>

namespace plume { struct RenderShader; }
namespace sfr {
class GuestMemory;
class NativeGraphics;
struct NativeShader {
    const ShaderCacheEntry* entry = nullptr;
    uint32_t references = 1;
    std::unique_ptr<plume::RenderShader> shader;
    ~NativeShader();
};
// Only memoize guest-object resolution, not bytecode or render-state-dependent
// specialization. The caller still reads both binding words on every draw.
// Handles remain owned by NativeShaders; no mutable guest storage is retained.
class NativeShaderObjectMemo {
public:
    struct Bindings {
        uint32_t vertex = 0, pixel = 0;
        explicit operator bool() const noexcept { return vertex && pixel; }
        bool operator==(const Bindings&) const = default;
    };
    void reset() noexcept { bindings_ = {}; }
    template<class Resolve>
    Bindings resolve(uint64_t manager, uint64_t generation, uint32_t device,
                     uint32_t vertex, uint32_t pixel, Resolve&& lookup) {
        const bool same_owner = bindings_ && manager_ == manager &&
            generation_ == generation && device_ == device;
        if (same_owner && vertex_ == vertex && pixel_ == pixel) return bindings_;
        // Preserve consume-time failure and its VS-before-PS resolution order.
        // An invalid pair never replaces a previously published valid pair.
        const uint32_t vs = same_owner && vertex_ == vertex ? bindings_.vertex
            : vertex ? lookup(vertex) : 0;
        if (!vs) return {};
        const uint32_t ps = same_owner && pixel_ == pixel ? bindings_.pixel
            : pixel ? lookup(pixel) : 0;
        if (!ps) return {};
        manager_ = manager;
        generation_ = generation;
        device_ = device;
        vertex_ = vertex;
        pixel_ = pixel;
        bindings_ = {vs, ps};
        return bindings_;
    }
private:
    uint64_t manager_ = 0, generation_ = 0;
    uint32_t device_ = 0, vertex_ = 0, pixel_ = 0;
    Bindings bindings_;
};
class NativeShaders {
public:
    NativeShaders(GuestMemory& memory, NativeGraphics& graphics);
    ~NativeShaders();
    uint32_t create(ShaderStage stage, uint32_t container);
    const NativeShader& get(uint32_t handle) const;
    bool owns(uint32_t handle) const noexcept;
    // The original CreateVertexShader/CreatePixelShader build the real guest
    // shader object; the game holds that object, and the native shader is
    // found through it. Call attach only for a successfully created original
    // object: its heap address may have belonged to a shader in an old scene.
    // Identical immutable stages share a native handle across guest objects.
    void attach(uint32_t handle, uint32_t object);
    bool owns_object(uint32_t object) const noexcept;
    uint32_t handle_of(uint32_t object) const;
    // Empty if a draw consumes a null/unknown binding. The draw retains its
    // existing error and unsupported-code behavior. Serialized with attach.
    NativeShaderObjectMemo::Bindings resolve_objects(uint32_t device, uint32_t vertex, uint32_t pixel) const;
    size_t size() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
