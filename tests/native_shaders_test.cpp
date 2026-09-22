#include "native_shaders.h"
#include "native_graphics.h"
#include "guest_memory.h"
#include <plume_render_interface.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok, const char* detail) { if (!ok) throw std::runtime_error(detail); }
template<class F> void rejects(F operation) {
    try { operation(); } catch (const sfr::RuntimeStop&) { return; }
    throw std::runtime_error("unsupported shader operation did not stop");
}
void run() {
    sfr::GuestMemory memory;
    sfr::NativeGraphics graphics;
    sfr::NativeShaders shaders(memory,graphics);
    constexpr uint32_t source=0x10000000;
    memory.map(source,0x100000);
    rejects([&]{ shaders.create(sfr::ShaderStage::vertex,source); });
    require(!graphics.initialized() && shaders.size()==0,"rejection has no native side effects");
    graphics.initialize();
    rejects([&]{ shaders.create(sfr::ShaderStage::vertex,0xFFFFFFF8); });
    memory.store<uint32_t>(source,0x102A1101);
    memory.store<uint32_t>(source+4,0xFFFFFFFC);
    memory.store<uint32_t>(source+8,0x100);
    rejects([&]{ shaders.create(sfr::ShaderStage::vertex,source); });
    rejects([&]{ shaders.get(0); });
    require(shaders.size()==0,"invalid extent creates no shader");
    const auto cache=sfr::compiled_shader_cache();
    // The prepared cache is DXIL: Vulkan (and Linux) never read it.
    if(cache.empty() || sfr::selected_graphics_backend()==sfr::GraphicsBackend::vulkan) {
        std::cout << "No prepared DXIL shader cache in use: negative checks only\n";
        return;
    }
    require(cache.size()==68 || cache.size()==74,"complete basic or combined original shader cache");
    const size_t expected_per_stage=cache.size()/2;
    size_t vertex=0,pixel=0;
    for(const auto& entry:cache) {
        for(size_t i=0;i<entry.source.size();++i) memory.store<uint8_t>(source+i,entry.source[i]);
        const auto wrong=entry.stage==sfr::ShaderStage::vertex?sfr::ShaderStage::pixel:sfr::ShaderStage::vertex;
        rejects([&]{ shaders.create(wrong,source); });
        // A valid-looking container with even one different instruction must
        // not acquire an unrelated cached shader.
        memory.store<uint8_t>(source+entry.source.size()-1,uint8_t(entry.source.back()^1));
        rejects([&]{ shaders.create(entry.stage,source); });
        memory.store<uint8_t>(source+entry.source.size()-1,entry.source.back());
        const uint32_t handle=shaders.create(entry.stage,source);
        require(handle && shaders.owns(handle),"created shader retained by native owner");
        rejects([&]{memory.load<uint32_t>(handle);});
        const auto& shader=shaders.get(handle);
        require(shader.entry==&entry,"exact original container entry retained");
        require(shader.references==1,"new native resource starts with its original reference count");
        if(entry.stage==sfr::ShaderStage::vertex) {
            ++vertex;
            require(entry.specialization_mask==0 && shader.shader,"vertex shader has actual Plume bytecode owner");
        } else {
            ++pixel;
            require(entry.specialization_mask==2 && !shader.shader && !entry.dxil.empty(),
                    "pixel library waits for observed alpha-test specialization state");
        }
    }
    require(vertex==expected_per_stage && pixel==expected_per_stage && shaders.size()==cache.size(),
            "all original shaders retained with exact stage counts");
    std::cout << "Original shaders: " << vertex << " native vertex stages, " << pixel << " retained pixel libraries\n";
}
}
int main() { try { run(); return 0; } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; } }
