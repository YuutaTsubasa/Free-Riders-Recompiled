#include "runtime_shader_cache.h"
#include "guest_memory.h"
#include "native_formats.h"
#include "loop_constants.h"
#include "native_graphics.h"
#include "vulkan_shader_source.h"
#include "shader_inputs.h"
#include "vertex_palette.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace sfr {
bool runtime_shader_translation = false;
namespace {
namespace fs = std::filesystem;

struct OwnedShader {
    std::vector<uint8_t> source, dxil, spirv;
    std::string failure;
    ShaderCacheEntry entry;
};

[[noreturn]] void failed(const std::string& reason) {
    throw RuntimeStop("runtime-shader", 0, reason);
}

// XenosRecomp's dxc-bin, relative to the working directory (the checkout):
// SPIR-V everywhere, and DXIL on Windows.
#if defined(_WIN32)
constexpr const char* default_dxc = "tools/XenosRecomp/thirdparty/dxc-bin/bin/x64/dxc.exe";
#elif defined(__APPLE__)
constexpr const char* default_dxc = "tools/XenosRecomp/thirdparty/dxc-bin/bin/x64/dxc-macos";
#else
constexpr const char* default_dxc = "tools/XenosRecomp/thirdparty/dxc-bin/bin/x64/dxc-linux";
#endif

fs::path setting(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return fs::path(value && *value ? value : fallback);
}

std::vector<uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

uint32_t load_be32(std::span<const uint8_t> data, size_t offset) {
    if (offset + 4 > data.size()) failed("shader container is truncated");
    return uint32_t(data[offset]) << 24 | uint32_t(data[offset + 1]) << 16 | uint32_t(data[offset + 2]) << 8 | data[offset + 3];
}

// Renames TEXCOORD4-12 vertex elements (shader_input_usage) in a vertex shader
// container; element words are address:12, usage:4, index:4 from bit 0.
// Containers already using a replacement name are left unchanged.
std::vector<uint8_t> rename_high_texcoords(std::span<const uint8_t> source) {
    std::vector<uint8_t> result(source.begin(), source.end());
    if (source.size() < 0x24 || (load_be32(source, 0) & 1) == 0) return result;  // pixel shader
    const size_t shader = load_be32(source, 0x18);
    const uint32_t first = load_be32(source, shader + 0x18), count = load_be32(source, shader + 0x1C);
    if (count > 64) failed("vertex shader declares too many elements");
    const auto element = [&](uint32_t i) { return shader + 0x24 + size_t(first + i) * 4; };
    std::vector<uint32_t> renamed;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t value = load_be32(source, element(i)), usage = (value >> 12) & 15, index = (value >> 16) & 15;
        const auto target = shader_input_usage(usage, index);
        if (target.usage == usage && target.index == index) continue;
        for (uint32_t j = 0; j < count; ++j) {
            const uint32_t other = load_be32(source, element(j));
            if (((other >> 12) & 15) == target.usage && ((other >> 16) & 15) == target.index) return result;
        }
        renamed.push_back(i);
    }
    for (const uint32_t i : renamed) {
        const uint32_t value = load_be32(source, element(i));
        const auto target = shader_input_usage((value >> 12) & 15, (value >> 16) & 15);
        const uint32_t updated = (value & ~0xFF000u) | target.usage << 12 | target.index << 16;
        for (int b = 0; b < 4; ++b) result[element(i) + b] = uint8_t(updated >> (24 - 8 * b));
    }
    return result;
}

void write_file(const fs::path& path, std::span<const uint8_t> data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    if (!out) failed("cannot write " + path.string());
}

// Runs a command through the shell with its output captured in log.
void run(const std::vector<fs::path>& arguments, const fs::path& log, const char* what) {
#ifdef _WIN32
    std::wstring command = L"\"";  // cmd /c strips one outer pair of quotes
    for (const auto& argument : arguments) command += L"\"" + argument.wstring() + L"\" ";
    command += L"> \"" + log.wstring() + L"\" 2>&1\"";
    if (_wsystem(command.c_str()) != 0) {
#else
    const auto quoted = [](const std::string& text) {
        std::string out = "'";
        for (const char c : text) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
        return out + "'";
    };
    // dxc-linux loads libdxcompiler.so from dxc-bin's lib directory (a
    // release's own: SFR_DXC_LIBRARY).
    const std::string libraries = setting("SFR_DXC_LIBRARY", "tools/XenosRecomp/thirdparty/dxc-bin/lib/x64").string();
#ifdef __APPLE__
    std::string command = "DYLD_LIBRARY_PATH=" + quoted(libraries) + " ";
#else
    std::string command = "LD_LIBRARY_PATH=" + quoted(libraries) + "${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} ";
#endif
    for (const auto& argument : arguments) command += quoted(argument.string()) + " ";
    command += "> " + quoted(log.string()) + " 2>&1";
    if (std::system(command.c_str()) != 0) {
#endif
        const auto output = read_file(log);
        failed(std::string(what) + " failed: " + std::string(output.begin(), output.end()));
    }
}

// The pinned shader_common.h plus one shared constant, g_ScreenSpaceScale at
// c20.xy (see SharedConstants::screen_space_scale). Written next to the cache.
fs::path extended_common_header() {
    const fs::path pinned = setting("SFR_SHADER_COMMON", "tools/XenosRecomp/XenosRecomp/shader_common.h");
    const fs::path extended = setting("SFR_RUNTIME_SHADER_CACHE", "out/shaders/runtime") / "shader_common_extended.h";
    const auto bytes = read_file(pinned);
    std::string text(bytes.begin(), bytes.end());
    const std::string anchor = "uint g_conditionalRenderingIndex : packoffset(c19.w);";
    const auto at = text.find(anchor);
    if (at == std::string::npos) failed("pinned shader_common.h lacks the shared constant block");
    text.replace(at, anchor.size(), anchor.substr(0, anchor.size() - 1) + "; \\\n    float2 g_ScreenSpaceScale : packoffset(c20.x);");
    // Two more push-constant addresses, for SPIR-V: the skinning palette and
    // the loop constants. They used to be read out of the shared constants
    // with a 64-bit vk::RawBufferLoad of their own, which is an indirection
    // in every skinned draw and an under-aligned load besides.
    const std::string push = "    uint64_t SharedConstants;";
    const auto push_at = text.find(push);
    if (push_at == std::string::npos) failed("pinned shader_common.h lacks the push constant block");
    text.replace(push_at, push.size(), push + "\n    uint64_t VertexPalette;\n    uint64_t LoopConstants;");
    std::ofstream(extended, std::ios::binary) << text;
    return extended;
}

// Drops the repeated vertex input declarations of a translated shader.
void dedupe_vertex_input_structure(const fs::path& hlsl) {
    const auto bytes = read_file(hlsl);
    const auto text = dedupe_vertex_inputs(std::string(bytes.begin(), bytes.end()));
    if (text) std::ofstream(hlsl, std::ios::binary | std::ios::trunc) << *text;
}

// Reads translated HLSL, rewrites its palette fetches and writes it back.
void add_vertex_palette(const VertexPalette& palette, const fs::path& hlsl) {
    const auto bytes = read_file(hlsl);
    const auto text = apply_vertex_palette(palette, std::string(bytes.begin(), bytes.end()));
    if (!text) failed("translated vertex shader does not read the palette once per fetch");
    std::ofstream(hlsl, std::ios::binary | std::ios::trunc) << *text;
}

// Declares the loop constants the title supplies at draw time, if the shader
// leaves any undeclared.
void add_loop_constant_buffer(const fs::path& hlsl) {
    const auto bytes = read_file(hlsl);
    const auto text = add_loop_constants(std::string(bytes.begin(), bytes.end()));
    if (text) std::ofstream(hlsl, std::ios::binary | std::ios::trunc) << *text;
}

// Screen-space vertex output (viewport transform disabled): pixels to clip
// space, before the half-pixel offset the translator appends.
void add_screen_space_mapping(const fs::path& hlsl) {
    const auto bytes = read_file(hlsl);
    std::string text(bytes.begin(), bytes.end());
    const std::string anchor = "\tif (g_ClipPlaneEnabled)";
    const auto at = text.rfind(anchor);
    if (at == std::string::npos) failed("translated vertex shader lacks its output epilogue");
    text.insert(at, "\tif (any(g_ScreenSpaceScale != 0.0)) output.oPos.xy = output.oPos.xy * g_ScreenSpaceScale"
                    " + float2(-1.0, 1.0) * output.oPos.w;\n");
    std::ofstream(hlsl, std::ios::binary | std::ios::trunc) << text;
}

uint64_t fnv1a(std::span<const uint8_t> data) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (const uint8_t byte : data) hash = (hash ^ byte) * 0x100000001b3ull;
    return hash;
}

// The shader pack (scripts/pack_shaders.py): "SFRSHPK1", u32 count, then per
// entry u32 stage, mask, source, DXIL and SPIR-V sizes and those bytes.
struct PackedShader {
    ShaderStage stage;
    uint32_t mask;
    std::vector<uint8_t> source, dxil, spirv;
};

std::unordered_multimap<uint64_t, PackedShader> load_shader_pack() {
    std::unordered_multimap<uint64_t, PackedShader> pack;
    const fs::path path = setting("SFR_SHADER_PACK", "out/shaders/shaders.pack");
    const auto bytes = read_file(path);
    if (bytes.empty()) return pack;
    size_t at = 0;
    const auto u32 = [&]() -> uint32_t {
        if (at + 4 > bytes.size()) throw RuntimeStop("shader-pack", 0, "the shader pack is truncated");
        const uint32_t value = uint32_t(bytes[at]) | uint32_t(bytes[at + 1]) << 8 | uint32_t(bytes[at + 2]) << 16 |
                               uint32_t(bytes[at + 3]) << 24;
        at += 4;
        return value;
    };
    const auto take = [&](uint32_t size) {
        if (size > bytes.size() - at) throw RuntimeStop("shader-pack", size, "the shader pack is truncated");
        std::vector<uint8_t> part(bytes.begin() + std::ptrdiff_t(at), bytes.begin() + std::ptrdiff_t(at + size));
        at += size;
        return part;
    };
    if (bytes.size() < 12 || std::string(bytes.begin(), bytes.begin() + 8) != "SFRSHPK1")
        throw RuntimeStop("shader-pack", 0, "not a shader pack: " + path.string());
    at = 8;
    const uint32_t count = u32();
    for (uint32_t i = 0; i < count; ++i) {
        PackedShader shader;
        const uint32_t stage = u32();
        if (stage > 1) throw RuntimeStop("shader-pack", stage, "unknown shader stage in the pack");
        shader.stage = ShaderStage(stage);
        shader.mask = u32();
        const uint32_t source = u32(), dxil = u32(), spirv = u32();
        shader.source = take(source);
        shader.dxil = take(dxil);
        shader.spirv = take(spirv);
        pack.emplace(fnv1a(shader.source), std::move(shader));
    }
    std::cerr << "RUNTIME_SHADER_PACK path=" << path.string() << " shaders=" << count << '\n';
    return pack;
}

const PackedShader* find_packed_shader(ShaderStage stage, std::span<const uint8_t> source, uint64_t hash) {
    static const auto pack = load_shader_pack();
    const auto [first, last] = pack.equal_range(hash);
    for (auto it = first; it != last; ++it)
        if (it->second.stage == stage && std::ranges::equal(it->second.source, source)) return &it->second;
    return nullptr;
}
}

const ShaderCacheEntry& runtime_shader(ShaderStage stage, std::span<const uint8_t> source) {
    static std::mutex lock;
    static std::deque<OwnedShader> shaders;
    static std::unordered_map<uint64_t, std::vector<size_t>> by_hash;
    std::lock_guard guard(lock);

    const uint64_t hash = fnv1a(source);
    for (const size_t index : by_hash[hash]) {
        const auto& owned = shaders[index];
        if (owned.entry.stage == stage && std::ranges::equal(owned.source, source)) return owned.entry;
    }

    // A packed shader (scripts/pack_shaders.py) needs no translator or DXC.
    const bool vulkan_backend = selected_graphics_backend() == GraphicsBackend::vulkan;
    if (const PackedShader* packed = find_packed_shader(stage, source, hash)) {
        const auto& code = vulkan_backend ? packed->spirv : packed->dxil;
        if (!code.empty()) {
            auto& owned = shaders.emplace_back();
            owned.source.assign(source.begin(), source.end());
            by_hash[hash].push_back(shaders.size() - 1);
            (vulkan_backend ? owned.spirv : owned.dxil) = code;
            const auto palette = vertex_palette(rename_high_texcoords(source));
            owned.entry = ShaderCacheEntry{owned.source, owned.dxil, stage, packed->mask,
                                           palette ? palette->entry_float4s : 0, owned.spirv};
            return owned.entry;
        }
    }

    char name[32];
    // "v2" added the screen-space mapping to vertex shaders; "v3" renames
    // TEXCOORD4-7 vertex elements before translation; "v4" declares the
    // skinning palette fetches the vertex declaration leaves out and the
    // loop constants the title sets at draw time; "v5" renames TEXCOORD8-12
    // as well and drops repeated declarations of one input; "v6" compiles the
    // DXIL with dxc-bin (not the Windows SDK's DXC), whose linker the game
    // uses; "v7" inlines a header that declares the palette and loop constant
    // push constants, which the SPIR-V reads instead of addresses kept in the
    // shared constants (docs/vulkan-push-constants.md).
    // "v8" loads push constant addresses as uint2 for Adreno and explicitly
    // targets Vulkan 1.2 for PhysicalStorageBuffer64.
    std::snprintf(name, sizeof name, "v8-%016llx-%zu", static_cast<unsigned long long>(hash), source.size());
    const fs::path folder = setting("SFR_RUNTIME_SHADER_CACHE", "out/shaders/runtime") / name;
    const fs::path original = folder / "original.bin", hlsl = folder / "shader.hlsl",
                   dxil = folder / "shader.dxil", mask_file = folder / "specialization_mask.txt",
                   spirv = folder / "shader.spv";
    // Vulkan builds its SPIR-V from the translated HLSL; D3D12 needs the DXIL.
    const bool vulkan = selected_graphics_backend() == GraphicsBackend::vulkan;
    std::error_code error;
    fs::create_directories(folder, error);
    if (error) failed("cannot create " + folder.string());

    const fs::path failure_file = folder / "untranslatable.txt";
    const bool same_source = read_file(original) == std::vector<uint8_t>(source.begin(), source.end());
    const bool cached = same_source && fs::exists(vulkan ? hlsl : dxil) && fs::exists(mask_file);
    auto& owned = shaders.emplace_back();
    owned.source.assign(source.begin(), source.end());
    by_hash[hash].push_back(shaders.size() - 1);
    // A shader the pinned translator rejects is still created: the original
    // object exists for the game, and draws with it report the reason.
    // The skinning palette the shader fetches, if any: the declaration the
    // translator needs is added to its input, and the draw has to bind the
    // buffer behind it, so it is worked out again for a cached shader too.
    const auto prepared = rename_high_texcoords(source);
    const auto palette = vertex_palette(prepared);
    const uint32_t palette_float4s = palette ? palette->entry_float4s : 0;
    const auto untranslatable = [&](const std::string& reason) -> const ShaderCacheEntry& {
        owned.failure = reason;
        owned.entry = ShaderCacheEntry{owned.source, {}, stage, 0};
        std::ofstream(failure_file) << reason;
        std::cerr << "RUNTIME_SHADER stage=" << (stage == ShaderStage::vertex ? "vertex" : "pixel")
                  << " source_bytes=" << source.size() << " untranslatable=1 key=" << name
                  << " reason=" << reason << '\n';
        return owned.entry;
    };
    if (same_source && fs::exists(failure_file)) {
        const auto text = read_file(failure_file);
        return untranslatable(std::string(text.begin(), text.end()));
    }
    uint32_t mask = 0;
    if (cached) {
        std::ifstream(mask_file) >> mask;
    } else try {
        write_file(original, source);
        const fs::path input = folder / "translator_input.bin";
        write_file(input, palette ? palette->container : prepared);
        const fs::path translate_log = folder / "translate.log";
        run({setting("SFR_SHADER_TRANSLATOR", "out/tools/shader-translator/shader_translate.exe"),
             input, hlsl, extended_common_header()},
            translate_log, "shader translation");
        if (palette) add_vertex_palette(*palette, hlsl);
        if (stage == ShaderStage::vertex) {
            dedupe_vertex_input_structure(hlsl);
            add_loop_constant_buffer(hlsl);
        }
        if (stage == ShaderStage::vertex) add_screen_space_mapping(hlsl);
        const auto log = read_file(translate_log);
        const std::string text(log.begin(), log.end());
        const auto marker = text.find("spec_constants_mask=");
        if (marker == std::string::npos) failed("translator reported no specialization mask: " + text);
        mask = uint32_t(std::stoul(text.substr(marker + 20)));
        const bool vertex_reported = text.find("vertex HLSL") != std::string::npos;
        if (vertex_reported != (stage == ShaderStage::vertex)) failed("translator stage differs from container stage");
        if (!vulkan) {
        // Same profile as scripts/prepare_shaders.py: specialized shaders stay libraries.
        std::vector<fs::path> compile{setting("SFR_DXC", default_dxc)};
        if (mask) {
            compile.insert(compile.end(), {"-T", "lib_6_3"});
        } else {
            compile.insert(compile.end(), {"-T", stage == ShaderStage::vertex ? "vs_6_0" : "ps_6_0", "-E", "shaderMain"});
        }
        compile.insert(compile.end(), {"-HV", "2021", "-all-resources-bound", "-Wno-ignored-attributes",
                                       "-Qstrip_reflect", "-Qstrip_debug", "-Fo", dxil, hlsl});
        run(compile, folder / "compile.log", "shader compilation");
        }
        std::ofstream(mask_file) << mask;
    } catch (const RuntimeStop& error) {
        std::string reason = error.detail;
        for (char& c : reason) if (c == 10 || c == 13) c = 32;  // one log line
        return untranslatable(reason);
    }

    if (vulkan) {
        // SPIR-V, compiled once from the HLSL with XenosRecomp's dxc-bin; the
        // specialization constant stays a constant (set per pipeline).
        if (!fs::exists(spirv)) try {
            const auto bytes = read_file(hlsl);
            const auto text = vulkan_shader_source(std::string(bytes.begin(), bytes.end()));
            if (!text) failed("translated shader lacks the common header's SPIR-V branch");
            const fs::path vulkan_hlsl = folder / "shader.vulkan.hlsl";
            write_file(vulkan_hlsl, std::span(reinterpret_cast<const uint8_t*>(text->data()), text->size()));
            std::vector<fs::path> compile{setting("SFR_DXC_SPIRV", default_dxc),
                                          "-T", stage == ShaderStage::vertex ? "vs_6_0" : "ps_6_0", "-E", "shaderMain",
                                          "-HV", "2021", "-all-resources-bound", "-spirv", "-fvk-use-dx-layout",
                                          "-fspv-target-env=vulkan1.2"};
            if (stage == ShaderStage::vertex) compile.push_back("-fvk-invert-y");  // D3D clip space to Vulkan's
            compile.insert(compile.end(), {"-Qstrip_debug", "-Fo", spirv, vulkan_hlsl});
            run(compile, folder / "compile_spirv.log", "SPIR-V compilation");
        } catch (const RuntimeStop& error) {
            std::string reason = error.detail;
            for (char& c : reason) if (c == 10 || c == 13) c = 32;
            // Not recorded in untranslatable.txt: D3D12 shares this folder.
            owned.failure = reason;
            owned.entry = ShaderCacheEntry{owned.source, {}, stage, 0};
            std::cerr << "RUNTIME_SHADER stage=" << (stage == ShaderStage::vertex ? "vertex" : "pixel")
                      << " source_bytes=" << source.size() << " untranslatable=1 key=" << name
                      << " reason=" << reason << '\n';
            return owned.entry;
        }
        owned.spirv = read_file(spirv);
        if (owned.spirv.size() < 20) failed("compiled SPIR-V is missing: " + spirv.string());
    } else {
        owned.dxil = read_file(dxil);
        if (owned.dxil.size() < 32) failed("compiled shader is missing: " + dxil.string());
    }
    owned.entry = ShaderCacheEntry{owned.source, owned.dxil, stage, mask, palette_float4s, owned.spirv};
    std::cerr << "RUNTIME_SHADER stage=" << (stage == ShaderStage::vertex ? "vertex" : "pixel")
              << " source_bytes=" << source.size() << " dxil_bytes=" << owned.dxil.size()
              << " spirv_bytes=" << owned.spirv.size()
              << " specialization_mask=" << mask << " palette_float4s=" << palette_float4s
              << " cache=" << (cached ? "disk" : "translated")
              << " key=" << name << '\n';
    return owned.entry;
}
}
