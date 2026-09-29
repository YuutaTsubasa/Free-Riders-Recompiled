#include "native_shaders.h"
#include "native_graphics.h"
#include "guest_memory.h"
#include "runtime_shader_cache.h"
#include "shader_pack_format.h"
#include <plume_render_interface.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <fstream>
#include <vector>
#include <cstdlib>

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

// Exercise the actual native ownership path with the release pack, without
// checking proprietary shader sources into the repository.
void lifetime(const char* pack) {
#ifdef _WIN32
    _putenv_s("SFR_SHADER_PACK", pack);
#else
    setenv("SFR_SHADER_PACK", pack, 1);
#endif
    std::ifstream in(pack, std::ios::binary);
    require(bool(in), "cannot open lifetime test shader pack");
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in), {}};
    const auto count = sfr::shader_pack_count(bytes);
    size_t at = sfr::shader_pack_header_size;
    auto word = [&] {
        require(at + 4 <= bytes.size(), "truncated test pack");
        uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= uint32_t(bytes[at++]) << (i * 8);
        return value;
    };
    sfr::GuestMemory memory;
    sfr::NativeGraphics graphics;
    graphics.initialize();
    sfr::NativeShaders shaders(memory, graphics);
    sfr::runtime_shader_translation = true;
    constexpr uint32_t source = 0x10000000, object = 0x40566410;
    memory.map(source, 0x100000);
    uint32_t first = 0, second = 0;
    std::vector<uint8_t> first_source;
    sfr::ShaderStage first_stage{};
    for (uint32_t i = 0; i < count && !second; ++i) {
        const auto stage = sfr::ShaderStage(word());
        (void)word();
        const auto size = word(), dxil = word(), spirv = word();
        require(uint64_t(at) + size + dxil + spirv <= bytes.size(), "truncated test entry");
        if (size <= 0x100000 && (graphics.backend() == sfr::GraphicsBackend::vulkan ? spirv : dxil)) {
            for (uint32_t n = 0; n < size; ++n) memory.store<uint8_t>(source + n, bytes[at + n]);
            const auto handle = shaders.create(stage, source);
            if (!first) {
                first = handle;
                first_stage = stage;
                first_source.assign(bytes.begin() + at, bytes.begin() + at + size);
                shaders.attach(first, object);
                require(shaders.create(stage, source) == first,
                        "identical shader recreation must reuse the immutable native stage");
                shaders.attach(first, object + 0x1000);
                require(shaders.handle_of(object) == first && shaders.handle_of(object + 0x1000) == first,
                        "independent guest objects can share one immutable shader");
            } else if (handle != first) {
                second = handle;
                // Original creation just allocated a new object at the same
                // heap address after a scene unload (Issue #20).
                shaders.attach(second, object);
                require(shaders.handle_of(object) == second && shaders.handle_of(object + 0x1000) == first,
                        "recycled guest address must replace only its own shader binding");
            }
        }
        at += size + dxil + spirv;
    }
    require(first && second, "test pack needs two distinct usable shaders");
    for (size_t n = 0; n < first_source.size(); ++n) memory.store<uint8_t>(source + n, first_source[n]);
    const auto before = shaders.size();
    for (unsigned i = 0; i < 2100; ++i) {
        require(shaders.create(first_stage, source) == first, "reloading retains the shader identity");
        shaders.attach(first, object);
    }
    require(shaders.size() == before, "scene reloads cannot exhaust native shader handles");
    rejects([&] { shaders.attach(0, object); });
    rejects([&] { shaders.attach(first, object + 1); });
    require(shaders.handle_of(object) == first, "invalid attaches preserve the current mapping");
    std::cout << "Shader lifetime: address reuse, shared stages and 2100 reloads passed\n";
}
}
int main(int argc, char** argv) { try { if (argc == 2) lifetime(argv[1]); else run(); return 0; } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; } }
