#include "native_shaders.h"
#include "guest_memory.h"
#include "native_graphics.h"
#include "runtime_shader_cache.h"
#include <plume_render_interface.h>
#include <algorithm>
#include <span>
#include <unordered_map>
#include <vector>
namespace sfr {
namespace {
// Handles fill 0x71800000..0x71FFFFFF, below the 0x72000000 file-handle namespace.
constexpr uint32_t handle_base=0x71800000, handle_stride=0x1000, maximum_shaders=2048;
[[noreturn]] void unsupported(uint32_t address,const char* detail) {
    throw RuntimeStop("native-shader",address,detail);
}
}
struct NativeShaders::Impl {
    GuestMemory& memory;
    NativeGraphics& graphics;
    std::vector<std::unique_ptr<NativeShader>> shaders;
    std::unordered_map<uint32_t, uint32_t> handles_by_object;
    std::unordered_map<uint32_t, uint32_t> objects_by_handle;
    Impl(GuestMemory& m,NativeGraphics& g) : memory(m),graphics(g) { shaders.reserve(maximum_shaders); }
};
NativeShader::~NativeShader() = default;
NativeShaders::NativeShaders(GuestMemory& m, NativeGraphics& g) : impl_(std::make_unique<Impl>(m,g)) {}
NativeShaders::~NativeShaders() = default;
uint32_t NativeShaders::create(ShaderStage stage, uint32_t container) {
    if(!impl_->graphics.initialized()) unsupported(container,"native device is not initialized");
    if(stage!=ShaderStage::vertex && stage!=ShaderStage::pixel) unsupported(container,"unknown shader stage");
    auto& memory=impl_->memory;
    if(!container || (container&3)) unsupported(container,"invalid shader container pointer");
    memory.check(container,12);
    const auto flags=memory.load<uint32_t>(container);
    const auto virtual_size=memory.load<uint32_t>(uint64_t(container)+4);
    const auto physical_size=memory.load<uint32_t>(uint64_t(container)+8);
    // Bit 0 selects the stage (as in XenosRecomp); other low flag bits vary
    // between shaders and do not affect translation.
    if((flags&0xFFFFFF00u)!=0x102A1100u || bool(flags&1)!=(stage==ShaderStage::vertex))
        unsupported(container,"shader container stage mismatch");
    const uint64_t size=uint64_t(virtual_size)+physical_size;
    if(virtual_size<36 || !physical_size || size>0x100000)
        unsupported(container,"unsupported shader container extent");
    memory.check(container,size);
    // Scalar reads retain GuestMemory's import/provider protection; a checked
    // raw host pointer alone would bypass those protections.
    std::vector<uint8_t> source(size);
    for(size_t i=0;i<source.size();++i) source[i]=memory.load<uint8_t>(uint64_t(container)+i);
    // The prepared cache holds DXIL only; Vulkan's shaders come from the
    // runtime cache.
    const bool vulkan=impl_->graphics.backend()==GraphicsBackend::vulkan;
    const auto cache=vulkan?std::span<const ShaderCacheEntry>():compiled_shader_cache();
    const auto prepared=std::find_if(cache.begin(),cache.end(),[&](const auto& entry) {
        return entry.stage==stage && std::ranges::equal(entry.source,source);
    });
    if(prepared==cache.end() && !runtime_shader_translation)
        unsupported(container,"original shader is absent from the prepared native cache");
    const ShaderCacheEntry* found=prepared!=cache.end()?&*prepared:&runtime_shader(stage,source);
    // An empty payload marks a runtime shader the translator rejected; draws check it.
    if(found->dxil.empty() && prepared!=cache.end()) unsupported(container,"prepared shader has no DXIL payload");
    if(impl_->shaders.size()==maximum_shaders) unsupported(container,"native shader handle capacity exhausted");
    const auto handle=handle_base+uint32_t(impl_->shaders.size())*handle_stride;
    if(!memory.available(handle,handle_stride)) unsupported(handle,"native shader handle address is occupied");
    auto next=std::make_unique<NativeShader>();
    next->entry=found;
    // D3D12 specialization libraries remain attached to their source entry:
    // linking requires observed guest render state, which creation does not
    // invent. SPIR-V keeps the constant and takes its value per pipeline.
    const auto code=found->code(vulkan);
    if(!code.empty() && (vulkan || !found->specialization_mask)) {
        next->shader=impl_->graphics.device().createShader(code.data(),code.size(),"shaderMain",
            vulkan?plume::RenderShaderFormat::SPIRV:plume::RenderShaderFormat::DXIL);
        if(!next->shader) unsupported(container,"native shader bytecode ownership failed");
    }
    // Reserve, without committing, so Xbox header access faults and future
    // guest allocations cannot accidentally make this opaque ID readable.
    memory.reserve(handle,handle_stride);
    impl_->shaders.emplace_back(std::move(next));
    return handle;
}
const NativeShader& NativeShaders::get(uint32_t handle) const {
    if(!owns(handle)) unsupported(handle,"unknown native shader");
    return *impl_->shaders[(handle-handle_base)/handle_stride];
}
bool NativeShaders::owns(uint32_t handle) const noexcept {
    return handle>=handle_base && (handle-handle_base)%handle_stride==0 &&
           (handle-handle_base)/handle_stride<impl_->shaders.size();
}
size_t NativeShaders::size() const noexcept { return impl_->shaders.size(); }
void NativeShaders::attach(uint32_t handle, uint32_t object) {
    if(!owns(handle)) unsupported(handle,"attaching an unknown native shader");
    if(!object || (object&3)) unsupported(object,"invalid original shader object");
    if(impl_->objects_by_handle.contains(handle) || impl_->handles_by_object.contains(object))
        unsupported(object,"native shader or original object is already attached");
    impl_->objects_by_handle.emplace(handle,object);
    impl_->handles_by_object.emplace(object,handle);
}
bool NativeShaders::owns_object(uint32_t object) const noexcept {
    return impl_->handles_by_object.contains(object);
}
uint32_t NativeShaders::handle_of(uint32_t object) const {
    const auto found=impl_->handles_by_object.find(object);
    if(found==impl_->handles_by_object.end()) unsupported(object,"original shader object has no native shader");
    return found->second;
}
}
