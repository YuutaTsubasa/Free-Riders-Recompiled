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
class NativeShaders {
public:
    NativeShaders(GuestMemory& memory, NativeGraphics& graphics);
    ~NativeShaders();
    uint32_t create(ShaderStage stage, uint32_t container);
    const NativeShader& get(uint32_t handle) const;
    bool owns(uint32_t handle) const noexcept;
    // The original CreateVertexShader/CreatePixelShader build the real guest
    // shader object; the game holds that object, and the native shader is
    // found through it. Each handle and object is attached exactly once.
    void attach(uint32_t handle, uint32_t object);
    bool owns_object(uint32_t object) const noexcept;
    uint32_t handle_of(uint32_t object) const;
    size_t size() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
