// d3d11_shader_source (Issue #359): a pixel shader shaped as the translator
// writes them (written here by hand: no game shader is in the repository) is
// rewritten for shader model 5.0 and, on Windows, compiled with d3dcompiler_47.
// With a directory argument (a runtime shader cache, out/shaders/runtime), every
// shader.hlsl under it is rewritten and compiled instead, and the failures listed.
#include "d3d11_shader_source.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#ifdef _WIN32
#include <windows.h>
#include <d3dcompiler.h>
#endif

namespace {
int failures = 0;
void require(bool condition, const char* what) {
    if (!condition) {
        std::cerr << "FAILED: " << what << '\n';
        ++failures;
    }
}

const char* const shader = R"hlsl(#define DEFINE_SHARED_CONSTANTS() \
    uint g_Booleans : packoffset(c16.x); \
    float g_AlphaThreshold : packoffset(c19.y);
uint g_SpecConstants();
#define SPEC_CONSTANT_ALPHA_TEST (1 << 1)
#define selectWrapper(a, b, c) select(a, b, c)
Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
Texture2DArray<float4> g_Texture2DArrayDescriptorHeap[] : register(t0, space1);
TextureCube<float4> g_TextureCubeDescriptorHeap[] : register(t0, space2);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);
RWStructuredBuffer<uint> g_ConditionalSurveyBuffer : register(u0, space4);
float2 sfrTexture2DDimensions(uint descriptor, float2 logicalScale)
{
    uint2 dimensions;
    g_Texture2DDescriptorHeap[descriptor & 0x7fffffffu].GetDimensions(dimensions.x, dimensions.y);
    return (descriptor & 0x80000000u) ? round(dimensions * logicalScale) : float2(dimensions);
}
float4 tfetch2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex & 0x7fffffffu];
    return texture.Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex],
                          texCoord + 0.5 / sfrTexture2DDimensions(resourceDescriptorIndex, float2(1, 1)));
}
#define getPixelCoord(index, coord) coord
#ifdef __spirv__
#elif defined(__air__)
#else
cbuffer PixelShaderConstants : register(b1, space4)
{
	float4 tint[2] : packoffset(c0);
#define tint(INDEX) selectWrapper((INDEX) < 2, tint[min(INDEX, 1)], 0.0)
};
cbuffer SharedConstants : register(b2, space4)
{
	uint picture_Texture2DDescriptorIndex : packoffset(c0.z);
	uint picture_Texture2DArrayDescriptorIndex : packoffset(c4.z);
	uint picture_TextureCubeDescriptorIndex : packoffset(c8.z);
	uint picture_SamplerDescriptorIndex : packoffset(c12.z);
	DEFINE_SHARED_CONSTANTS();
};
#endif
struct Interpolators
{
	float4 iPos : SV_Position;
	[[vk::location(0)]] float4 iTexCoord0 : TEXCOORD0;
};
float4 shaderMain(Interpolators input, in uint iFace : SV_IsFrontFace) : SV_Target0
{
	float4 r0 = input.iTexCoord0;
	float4 r1 = 0.0;
	uint pc = 0;
	while (true)
	{
		switch (pc)
		{
		case 0:
			r1 = tfetch2D(picture_Texture2DDescriptorIndex, picture_SamplerDescriptorIndex, r0.xy);
			if (r1.w < 0.5)
			{
				pc = 2;
				continue;
			}
		case 1:
			r1 = selectWrapper(r1 > 0.25, tint(0), tint(1));
		case 2:
			if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)
				clip(r1.w - g_AlphaThreshold);
			break;
		}
		break;
	}
	return r1;
}
)hlsl";

#ifdef _WIN32
// The shader compiled as the D3D11 backend does; the compiler's messages if not.
bool compiles(const std::string& text, const char* profile, std::string* errors) {
    using Compile = decltype(&D3DCompile);
    static const Compile compile = [] {
        HMODULE module = LoadLibraryW(L"d3dcompiler_47.dll");
        return module ? reinterpret_cast<Compile>(GetProcAddress(module, "D3DCompile")) : nullptr;
    }();
    if (!compile) {
        *errors = "d3dcompiler_47.dll is unavailable";
        return false;
    }
    const D3D_SHADER_MACRO macros[] = {{"SFR_SPEC_CONSTANTS", "2"}, {nullptr, nullptr}};
    ID3DBlob* code = nullptr;
    ID3DBlob* messages = nullptr;
    const HRESULT result = compile(text.data(), text.size(), nullptr, macros, nullptr, "shaderMain", profile,
                                   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &messages);
    if (messages) {
        *errors = std::string(static_cast<const char*>(messages->GetBufferPointer()), messages->GetBufferSize());
        messages->Release();
    }
    if (code) code->Release();
    return SUCCEEDED(result);
}
#endif
}

int main(int argc, char** argv) {
    if (argc > 1) {
        // Every shader of a runtime cache (local check with game files).
        int total = 0, failed = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(argv[1])) {
            if (entry.path().filename() != "shader.hlsl") continue;
            std::ifstream in(entry.path(), std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            std::ifstream log(entry.path().parent_path() / "translate.log");
            std::stringstream log_text;
            log_text << log.rdbuf();
            const char* profile = log_text.str().find("vertex HLSL") != std::string::npos ? "vs_5_0" : "ps_5_0";
            ++total;
            std::string why;
            const auto rewritten = sfr::d3d11_shader_source(text.str(), &why);
#ifdef _WIN32
            if (rewritten && compiles(*rewritten, profile, &why)) continue;
#else
            if (rewritten) continue;
#endif
            ++failed;
            std::cerr << entry.path().parent_path().filename().string() << ' ' << profile << ": " << why.substr(0, 300) << '\n';
        }
        std::cout << "d3d11 shaders: " << total - failed << " of " << total << " compiled\n";
        return failed ? 1 : 0;
    }
    std::string why;
    const auto rewritten = sfr::d3d11_shader_source(shader, &why);
    require(rewritten.has_value(), "the shader is understood");
    if (!rewritten) {
        std::cerr << why << '\n';
        return 1;
    }
    const std::string& text = *rewritten;
    const auto has = [&](const char* part) { return text.find(part) != std::string::npos; };
    require(has("g_Texture2DDescriptorHeap[16] : register(t0);") && has("g_Texture2DArrayDescriptorHeap[16] : register(t16);") &&
            has("g_TextureCubeDescriptorHeap[16] : register(t32);") && has("g_SamplerDescriptorHeap[16] : register(s0);"),
            "the heaps are the per-draw arrays");
    require(has("#define picture_Texture2DDescriptorIndex 2u") && has("#define picture_SamplerDescriptorIndex 2u") &&
            has("#define picture_TextureCubeDescriptorIndex 2u"), "each fetch constant reads its own element");
    require(!has("space") && !has("g_ConditionalSurveyBuffer") && !has("vk::"), "no register spaces, survey or vk attributes");
    require(has("(SFR_SPEC_CONSTANTS) & SPEC_CONSTANT_ALPHA_TEST") && !has("g_SpecConstants"), "the specialization is a macro");
    require(!has("switch (pc)") && has("if (pc == 2u)") && has("pc = 1u;"), "the dispatch loop is guarded blocks");
    require(has("float4 tint_sfrArray[2]") && has("tint_sfrArray[min(INDEX, 1)]"), "the array a macro reads is renamed");
    require(has("sfrSharedIndices[descriptor >> 2][descriptor & 3] & 0x80000000u"), "the resolved flag is read from the constants");
    require(text.find("cbuffer SharedConstants") < text.find("g_Texture2DDescriptorHeap[16]"),
            "the shared constants come before the helpers that read them");
    require(has("in bool iFace : SV_IsFrontFace") && has("sfrTexture.Sample"), "FXC's words");
#ifdef _WIN32
    std::string errors;
    require(compiles(text, "ps_5_0", &errors), "the rewritten shader compiles for shader model 5.0");
    if (!errors.empty()) std::cerr << errors << '\n';
#endif
    require(!sfr::d3d11_shader_source("float4 main() : SV_Target { return 0; }").has_value(), "a foreign shader is refused");
    if (!failures) std::cout << "d3d11_shader_source: all passed\n";
    return failures ? 1 : 0;
}
