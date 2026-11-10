// Plume's render interface on Direct3D 11 (plume_d3d11.h). Issue #359.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "plume_d3d11.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

namespace plume {
namespace {
DXGI_FORMAT toDXGI(RenderFormat format) {
    switch (format) {
    case RenderFormat::R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_TYPELESS;
    case RenderFormat::R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case RenderFormat::R32G32B32A32_UINT: return DXGI_FORMAT_R32G32B32A32_UINT;
    case RenderFormat::R32G32B32A32_SINT: return DXGI_FORMAT_R32G32B32A32_SINT;
    case RenderFormat::R32G32B32_TYPELESS: return DXGI_FORMAT_R32G32B32_TYPELESS;
    case RenderFormat::R32G32B32_FLOAT: return DXGI_FORMAT_R32G32B32_FLOAT;
    case RenderFormat::R32G32B32_UINT: return DXGI_FORMAT_R32G32B32_UINT;
    case RenderFormat::R32G32B32_SINT: return DXGI_FORMAT_R32G32B32_SINT;
    case RenderFormat::R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    case RenderFormat::R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case RenderFormat::R16G16B16A16_UNORM: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case RenderFormat::R16G16B16A16_UINT: return DXGI_FORMAT_R16G16B16A16_UINT;
    case RenderFormat::R16G16B16A16_SNORM: return DXGI_FORMAT_R16G16B16A16_SNORM;
    case RenderFormat::R16G16B16A16_SINT: return DXGI_FORMAT_R16G16B16A16_SINT;
    case RenderFormat::R32G32_TYPELESS: return DXGI_FORMAT_R32G32_TYPELESS;
    case RenderFormat::R32G32_FLOAT: return DXGI_FORMAT_R32G32_FLOAT;
    case RenderFormat::R32G32_UINT: return DXGI_FORMAT_R32G32_UINT;
    case RenderFormat::R32G32_SINT: return DXGI_FORMAT_R32G32_SINT;
    case RenderFormat::R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case RenderFormat::R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case RenderFormat::R8G8B8A8_UINT: return DXGI_FORMAT_R8G8B8A8_UINT;
    case RenderFormat::R8G8B8A8_SNORM: return DXGI_FORMAT_R8G8B8A8_SNORM;
    case RenderFormat::R8G8B8A8_SINT: return DXGI_FORMAT_R8G8B8A8_SINT;
    case RenderFormat::B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case RenderFormat::R16G16_TYPELESS: return DXGI_FORMAT_R16G16_TYPELESS;
    case RenderFormat::R16G16_FLOAT: return DXGI_FORMAT_R16G16_FLOAT;
    case RenderFormat::R16G16_UNORM: return DXGI_FORMAT_R16G16_UNORM;
    case RenderFormat::R16G16_UINT: return DXGI_FORMAT_R16G16_UINT;
    case RenderFormat::R16G16_SNORM: return DXGI_FORMAT_R16G16_SNORM;
    case RenderFormat::R16G16_SINT: return DXGI_FORMAT_R16G16_SINT;
    case RenderFormat::R32_TYPELESS: return DXGI_FORMAT_R32_TYPELESS;
    case RenderFormat::D32_FLOAT: return DXGI_FORMAT_R32_TYPELESS;
    case RenderFormat::D32_FLOAT_S8_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
    case RenderFormat::D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
    case RenderFormat::R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case RenderFormat::R32_UINT: return DXGI_FORMAT_R32_UINT;
    case RenderFormat::R32_SINT: return DXGI_FORMAT_R32_SINT;
    case RenderFormat::R8G8_TYPELESS: return DXGI_FORMAT_R8G8_TYPELESS;
    case RenderFormat::R8G8_UNORM: return DXGI_FORMAT_R8G8_UNORM;
    case RenderFormat::R8G8_UINT: return DXGI_FORMAT_R8G8_UINT;
    case RenderFormat::R8G8_SNORM: return DXGI_FORMAT_R8G8_SNORM;
    case RenderFormat::R8G8_SINT: return DXGI_FORMAT_R8G8_SINT;
    case RenderFormat::R16_TYPELESS: return DXGI_FORMAT_R16_TYPELESS;
    case RenderFormat::R16_FLOAT: return DXGI_FORMAT_R16_FLOAT;
    case RenderFormat::D16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
    case RenderFormat::R16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case RenderFormat::R16_UINT: return DXGI_FORMAT_R16_UINT;
    case RenderFormat::R16_SNORM: return DXGI_FORMAT_R16_SNORM;
    case RenderFormat::R16_SINT: return DXGI_FORMAT_R16_SINT;
    case RenderFormat::R8_TYPELESS: return DXGI_FORMAT_R8_TYPELESS;
    case RenderFormat::R8_UNORM: return DXGI_FORMAT_R8_UNORM;
    case RenderFormat::R8_UINT: return DXGI_FORMAT_R8_UINT;
    case RenderFormat::R8_SNORM: return DXGI_FORMAT_R8_SNORM;
    case RenderFormat::R8_SINT: return DXGI_FORMAT_R8_SINT;
    case RenderFormat::BC1_TYPELESS: return DXGI_FORMAT_BC1_TYPELESS;
    case RenderFormat::BC1_UNORM: return DXGI_FORMAT_BC1_UNORM;
    case RenderFormat::BC1_UNORM_SRGB: return DXGI_FORMAT_BC1_UNORM_SRGB;
    case RenderFormat::BC2_TYPELESS: return DXGI_FORMAT_BC2_TYPELESS;
    case RenderFormat::BC2_UNORM: return DXGI_FORMAT_BC2_UNORM;
    case RenderFormat::BC2_UNORM_SRGB: return DXGI_FORMAT_BC2_UNORM_SRGB;
    case RenderFormat::BC3_TYPELESS: return DXGI_FORMAT_BC3_TYPELESS;
    case RenderFormat::BC3_UNORM: return DXGI_FORMAT_BC3_UNORM;
    case RenderFormat::BC3_UNORM_SRGB: return DXGI_FORMAT_BC3_UNORM_SRGB;
    case RenderFormat::BC4_TYPELESS: return DXGI_FORMAT_BC4_TYPELESS;
    case RenderFormat::BC4_UNORM: return DXGI_FORMAT_BC4_UNORM;
    case RenderFormat::BC4_SNORM: return DXGI_FORMAT_BC4_SNORM;
    case RenderFormat::BC5_TYPELESS: return DXGI_FORMAT_BC5_TYPELESS;
    case RenderFormat::BC5_UNORM: return DXGI_FORMAT_BC5_UNORM;
    case RenderFormat::BC5_SNORM: return DXGI_FORMAT_BC5_SNORM;
    case RenderFormat::BC6H_TYPELESS: return DXGI_FORMAT_BC6H_TYPELESS;
    case RenderFormat::BC6H_UF16: return DXGI_FORMAT_BC6H_UF16;
    case RenderFormat::BC6H_SF16: return DXGI_FORMAT_BC6H_SF16;
    case RenderFormat::BC7_TYPELESS: return DXGI_FORMAT_BC7_TYPELESS;
    case RenderFormat::BC7_UNORM: return DXGI_FORMAT_BC7_UNORM;
    case RenderFormat::BC7_UNORM_SRGB: return DXGI_FORMAT_BC7_UNORM_SRGB;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

// A typeless depth format as a shader reads it, and as a target.
DXGI_FORMAT toShaderView(RenderFormat format) {
    switch (format) {
    case RenderFormat::D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case RenderFormat::D32_FLOAT_S8_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case RenderFormat::D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case RenderFormat::D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    default: return toDXGI(format);
    }
}

DXGI_FORMAT toDepthView(RenderFormat format) {
    switch (format) {
    case RenderFormat::D32_FLOAT: return DXGI_FORMAT_D32_FLOAT;
    case RenderFormat::D32_FLOAT_S8_UINT: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    case RenderFormat::D24_UNORM_S8_UINT: return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case RenderFormat::D16_UNORM: return DXGI_FORMAT_D16_UNORM;
    default: return toDXGI(format);
    }
}

bool isDepth(RenderFormat format) {
    return format == RenderFormat::D32_FLOAT || format == RenderFormat::D32_FLOAT_S8_UINT ||
           format == RenderFormat::D24_UNORM_S8_UINT || format == RenderFormat::D16_UNORM;
}

D3D11_BLEND toD3D11(RenderBlend blend, D3D11_BLEND fallback) {
    switch (blend) {
    case RenderBlend::ZERO: return D3D11_BLEND_ZERO;
    case RenderBlend::ONE: return D3D11_BLEND_ONE;
    case RenderBlend::SRC_COLOR: return D3D11_BLEND_SRC_COLOR;
    case RenderBlend::INV_SRC_COLOR: return D3D11_BLEND_INV_SRC_COLOR;
    case RenderBlend::SRC_ALPHA: return D3D11_BLEND_SRC_ALPHA;
    case RenderBlend::INV_SRC_ALPHA: return D3D11_BLEND_INV_SRC_ALPHA;
    case RenderBlend::DEST_ALPHA: return D3D11_BLEND_DEST_ALPHA;
    case RenderBlend::INV_DEST_ALPHA: return D3D11_BLEND_INV_DEST_ALPHA;
    case RenderBlend::DEST_COLOR: return D3D11_BLEND_DEST_COLOR;
    case RenderBlend::INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_COLOR;
    case RenderBlend::SRC_ALPHA_SAT: return D3D11_BLEND_SRC_ALPHA_SAT;
    case RenderBlend::BLEND_FACTOR: return D3D11_BLEND_BLEND_FACTOR;
    case RenderBlend::INV_BLEND_FACTOR: return D3D11_BLEND_INV_BLEND_FACTOR;
    case RenderBlend::SRC1_COLOR: return D3D11_BLEND_SRC1_COLOR;
    case RenderBlend::INV_SRC1_COLOR: return D3D11_BLEND_INV_SRC1_COLOR;
    case RenderBlend::SRC1_ALPHA: return D3D11_BLEND_SRC1_ALPHA;
    case RenderBlend::INV_SRC1_ALPHA: return D3D11_BLEND_INV_SRC1_ALPHA;
    default: return fallback;
    }
}

template <typename T, typename R>
T cast_or(R value, T fallback) {
    return value == R::UNKNOWN ? fallback : T(int(value));
}

D3D11_FILTER toFilter(const RenderSamplerDesc& desc) {
    if (desc.anisotropyEnabled) return desc.comparisonEnabled ? D3D11_FILTER_COMPARISON_ANISOTROPIC : D3D11_FILTER_ANISOTROPIC;
    const bool minLinear = desc.minFilter == RenderFilter::LINEAR, magLinear = desc.magFilter == RenderFilter::LINEAR,
               mipLinear = desc.mipmapMode == RenderMipmapMode::LINEAR;
    const int filter = (mipLinear ? 0x1 : 0) | (magLinear ? 0x4 : 0) | (minLinear ? 0x10 : 0) | (desc.comparisonEnabled ? 0x80 : 0);
    return D3D11_FILTER(filter);
}

D3D11_PRIMITIVE_TOPOLOGY toTopology(RenderPrimitiveTopology topology) {
    switch (topology) {
    case RenderPrimitiveTopology::POINT_LIST: return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
    case RenderPrimitiveTopology::LINE_LIST: return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    case RenderPrimitiveTopology::LINE_STRIP: return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case RenderPrimitiveTopology::TRIANGLE_STRIP: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    default: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

std::string hresult(HRESULT value) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%08lx", static_cast<unsigned long>(value));
    return text;
}

uint64_t fnv1a(const void* data, size_t size, uint64_t hash = 0xcbf29ce484222325ull) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 0x100000001b3ull;
    return hash;
}

using D3DCompileProc = decltype(&D3DCompile);
D3DCompileProc compiler() {
    // d3dcompiler_47 is part of Windows 8.1 and later (and of Windows 7's
    // updates); System32's own, by full path.
    static const D3DCompileProc compile = [] {
        wchar_t system[MAX_PATH];
        const UINT length = GetSystemDirectoryW(system, MAX_PATH);
        HMODULE module = length ? LoadLibraryW((std::wstring(system, length) + L"\\d3dcompiler_47.dll").c_str()) : nullptr;
        if (!module) module = LoadLibraryW(L"d3dcompiler_47.dll");
        return module ? reinterpret_cast<D3DCompileProc>(GetProcAddress(module, "D3DCompile")) : nullptr;
    }();
    return compile;
}

std::vector<uint8_t> compile(const std::string& source, const char* entry, const char* profile,
                             const std::vector<D3D_SHADER_MACRO>& macros, std::string* errors) {
    const auto compile = compiler();
    if (!compile) {
        if (errors) *errors = "d3dcompiler_47.dll is unavailable";
        return {};
    }
    ComPtr<ID3DBlob> code, messages;
    const HRESULT result = compile(source.data(), source.size(), nullptr, macros.empty() ? nullptr : macros.data(),
                                   nullptr, entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &messages);
    if (FAILED(result) || !code) {
        if (errors && messages)
            *errors = std::string(static_cast<const char*>(messages->GetBufferPointer()), messages->GetBufferSize());
        return {};
    }
    const auto* bytes = static_cast<const uint8_t*>(code->GetBufferPointer());
    return {bytes, bytes + code->GetBufferSize()};
}

// SFR_D3D11_SHADER_CACHE: the folder compiled shaders are kept in.
std::filesystem::path shaderCacheFolder() {
    const char* setting = std::getenv("SFR_D3D11_SHADER_CACHE");
    if (setting && *setting) return setting;
    const char* runtime = std::getenv("SFR_RUNTIME_SHADER_CACHE");
    return std::filesystem::path(runtime && *runtime ? runtime : "out/shaders/runtime") / "d3d11";
}

std::atomic<uint32_t> precompiledHits{0};
}

const std::vector<uint8_t>* (*D3D11PrecompiledShader)(const std::string& key) = nullptr;

std::string D3D11ShaderKey(std::span<const uint8_t> source, const std::string& entry, const char* profile,
                           uint32_t specConstants) {
    char key[128];
    std::snprintf(key, sizeof key, "%016llx-%zu-%s-%s-%u",
                  static_cast<unsigned long long>(fnv1a(source.data(), source.size())), source.size(), entry.c_str(),
                  profile, specConstants);
    return key;
}

std::vector<uint8_t> D3D11CompileShader(std::span<const uint8_t> source, const std::string& entry, const char* profile,
                                        uint32_t specConstants, std::string* errors) {
    const std::string text(source.begin(), source.end());
    const std::string spec = std::to_string(specConstants);
    const std::vector<D3D_SHADER_MACRO> macros{{"SFR_SPEC_CONSTANTS", spec.c_str()}, {nullptr, nullptr}};
    return compile(text, entry.c_str(), profile, macros, errors);
}

// What the immediate context has bound, as far as this submission knows, so
// that a command binding what is bound already costs nothing. Everything
// starts unknown at each submission (presentation and resizing touch the
// context between them).
struct D3D11Bound {
    template <typename T>
    static T* unknown() { return reinterpret_cast<T*>(uintptr_t(1)); }
    struct Constants {
        ID3D11Buffer* buffer;
        UINT first, count;  // count 0: bound without an offset
        bool operator==(const Constants&) const = default;
    };
    struct Vertices {
        ID3D11Buffer* buffer;
        UINT stride, offset;
        bool operator==(const Vertices&) const = default;
    };
    ID3D11VertexShader* vs;
    ID3D11GeometryShader* gs;
    ID3D11PixelShader* ps;
    ID3D11InputLayout* inputLayout;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    ID3D11RasterizerState* rasterizer;
    ID3D11BlendState* blend;
    ID3D11DepthStencilState* depthStencil;
    UINT stencilReference;
    // views and samplers [stage]: 0 vertex, 1 pixel; constants: 0 vertex,
    // 1 geometry, 2 pixel.
    std::array<std::array<ID3D11ShaderResourceView*, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT>, 2> views;
    std::array<std::array<ID3D11SamplerState*, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT>, 2> samplers;
    std::array<std::array<Constants, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT>, 3> constants;
    std::array<Vertices, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertices;
    ID3D11Buffer* indexBuffer;
    DXGI_FORMAT indexFormat;
    UINT indexOffset;
    const D3D11Framebuffer* framebuffer;
    bool viewportsKnown, scissorsKnown;
    D3D11Bound() { forget(); }
    void forgetViews() {
        for (auto& stage : views) stage.fill(unknown<ID3D11ShaderResourceView>());
    }
    void forgetPipeline() {
        vs = unknown<ID3D11VertexShader>();
        gs = unknown<ID3D11GeometryShader>();
        ps = unknown<ID3D11PixelShader>();
        inputLayout = unknown<ID3D11InputLayout>();
        topology = D3D11_PRIMITIVE_TOPOLOGY(-1);
        rasterizer = unknown<ID3D11RasterizerState>();
        blend = unknown<ID3D11BlendState>();
        depthStencil = unknown<ID3D11DepthStencilState>();
        stencilReference = ~0u;
    }
    void forget() {
        forgetPipeline();
        forgetViews();
        for (auto& stage : samplers) stage.fill(unknown<ID3D11SamplerState>());
        for (auto& stage : constants) stage.fill({unknown<ID3D11Buffer>(), 0, 0});
        vertices.fill({unknown<ID3D11Buffer>(), 0, 0});
        indexBuffer = unknown<ID3D11Buffer>();
        indexFormat = DXGI_FORMAT_UNKNOWN;
        indexOffset = ~0u;
        framebuffer = unknown<const D3D11Framebuffer>();
        viewportsKnown = scissorsKnown = false;
    }
};

struct D3D11Replay {
    D3D11Device* device;
    ID3D11DeviceContext* context;
    ID3D11DeviceContext1* context1;
    const D3D11GraphicsPipeline* pipeline = nullptr;
    const D3D11Framebuffer* framebuffer = nullptr;
    std::vector<D3D11_VIEWPORT> viewports;
    std::vector<D3D11_RECT> scissors;
    uint32_t stencilReference = 0;
    bool stencilOverride = false;
    D3D11Bound bound;
    // SFR_D3D11_TRACE=<n>: the n-th submission with draws logs each draw's
    // state and how many samples it wrote (an occlusion query a draw).
    bool trace = false;
    uint32_t traced = 0;
    uint32_t draws = 0;
    // Binding calls made, and skipped as bound already (NATIVE_D3D11_REPLAY).
    uint32_t calls = 0, skipped = 0;
    // Constants copied at draws (without constant buffer offsets).
    uint64_t windowBytes = 0;
    D3D11Replay(D3D11Device* device, ID3D11DeviceContext* context, ID3D11DeviceContext1* context1)
        : device(device), context(context), context1(context1) {}
    template <typename Draw>
    void draw(const char* kind, uint32_t count, Draw&& issue) {
        ++draws;
        if (!trace) {
            issue();
            return;
        }
        ComPtr<ID3D11Query> query;
        D3D11_QUERY_DESC desc{D3D11_QUERY_OCCLUSION, 0};
        device->d3d->CreateQuery(&desc, &query);
        context->Begin(query.Get());
        issue();
        context->End(query.Get());
        UINT64 samples = 0;
        while (context->GetData(query.Get(), &samples, sizeof samples, 0) == S_FALSE) {}
        ID3D11RenderTargetView* targets[2] = {};
        ID3D11DepthStencilView* depth = nullptr;
        context->OMGetRenderTargets(2, targets, &depth);
        D3D11_VIEWPORT viewport{};
        UINT viewportCount = 1;
        context->RSGetViewports(&viewportCount, &viewport);
        D3D11_RECT scissor{};
        UINT scissorCount = 1;
        context->RSGetScissorRects(&scissorCount, &scissor);
        ID3D11Buffer* constants[5] = {};
        UINT first[5] = {}, num[5] = {};
        if (context1) context1->VSGetConstantBuffers1(0, 5, constants, first, num);
        ID3D11ShaderResourceView* views[3] = {};
        context->PSGetShaderResources(0, 3, views);
        ID3D11SamplerState* sampler = nullptr;
        context->PSGetSamplers(0, 1, &sampler);
        ID3D11Buffer* vertices = nullptr;
        UINT stride = 0, offset = 0;
        context->IAGetVertexBuffers(0, 1, &vertices, &stride, &offset);
        std::cerr << "D3D11_TRACE draw=" << traced++ << ' ' << kind << '=' << count << " samples=" << samples
                  << " valid=" << (pipeline && pipeline->valid) << " gs=" << (pipeline && pipeline->gs)
                  << " rtv=" << (targets[0] != nullptr) << " dsv=" << (depth != nullptr) << " viewport=" << viewport.TopLeftX
                  << ',' << viewport.TopLeftY << ',' << viewport.Width << 'x' << viewport.Height << ',' << viewport.MinDepth
                  << '-' << viewport.MaxDepth << " scissor=" << scissor.left << ',' << scissor.top << ',' << scissor.right
                  << ',' << scissor.bottom << " cb=";
        for (int i = 0; i < 5; ++i) std::cerr << (constants[i] ? 1 : 0) << ':' << first[i] << '/' << num[i] << ' ';
        std::cerr << "srv=" << (views[0] != nullptr) << (views[1] != nullptr) << (views[2] != nullptr)
                  << " sampler=" << (sampler != nullptr) << " vb=" << (vertices != nullptr) << '/' << stride << '/' << offset
                  << '\n';
        for (auto* item : targets) if (item) item->Release();
        if (depth) depth->Release();
        for (auto* item : constants) if (item) item->Release();
        for (auto* item : views) if (item) item->Release();
        if (sampler) sampler->Release();
        if (vertices) vertices->Release();
    }
    // Each set below calls D3D11 only for what differs from what is bound.
    template <typename T, typename Value>
    bool change(T& slot, const Value& value) {
        if (slot == value) {
            ++skipped;
            return false;
        }
        slot = value;
        ++calls;
        return true;
    }
    void applyPipeline() {
        if (!pipeline) return;
        if (change(bound.vs, pipeline->vs.Get())) context->VSSetShader(bound.vs, nullptr, 0);
        if (change(bound.gs, pipeline->gs.Get())) context->GSSetShader(bound.gs, nullptr, 0);
        if (change(bound.ps, pipeline->ps.Get())) context->PSSetShader(bound.ps, nullptr, 0);
        if (change(bound.inputLayout, pipeline->inputLayout.Get())) context->IASetInputLayout(bound.inputLayout);
        if (change(bound.topology, pipeline->topology)) context->IASetPrimitiveTopology(bound.topology);
        if (change(bound.rasterizer, pipeline->rasterizer.Get())) context->RSSetState(bound.rasterizer);
        // Vulkan's pipelines and D3D12's lists leave the blend constant at 0.
        static const float factor[4] = {0, 0, 0, 0};
        if (change(bound.blend, pipeline->blend.Get())) context->OMSetBlendState(bound.blend, factor, 0xFFFFFFFFu);
        const UINT reference = stencilOverride ? stencilReference : pipeline->stencilReference;
        if (bound.depthStencil != pipeline->depthStencil.Get() || bound.stencilReference != reference) {
            bound.depthStencil = pipeline->depthStencil.Get();
            bound.stencilReference = reference;
            ++calls;
            context->OMSetDepthStencilState(bound.depthStencil, reference);
        } else {
            ++skipped;
        }
    }
    // Views of slots [first, first + count) of a stage (0 vertex, 1 pixel):
    // one call over the range that differs.
    void setViews(int stage, UINT first, UINT count, ID3D11ShaderResourceView* const* views) {
        auto& slots = bound.views[stage];
        UINT low = UINT_MAX, high = 0;
        for (UINT i = 0; i < count; ++i)
            if (slots[first + i] != views[i]) {
                low = std::min(low, i);
                high = i;
                slots[first + i] = views[i];
            }
        if (low == UINT_MAX) {
            ++skipped;
            return;
        }
        ++calls;
        if (stage == 0) context->VSSetShaderResources(first + low, high - low + 1, views + low);
        else context->PSSetShaderResources(first + low, high - low + 1, views + low);
    }
    void setSamplers(int stage, UINT first, UINT count, ID3D11SamplerState* const* states) {
        auto& slots = bound.samplers[stage];
        UINT low = UINT_MAX, high = 0;
        for (UINT i = 0; i < count; ++i)
            if (slots[first + i] != states[i]) {
                low = std::min(low, i);
                high = i;
                slots[first + i] = states[i];
            }
        if (low == UINT_MAX) {
            ++skipped;
            return;
        }
        ++calls;
        if (stage == 0) context->VSSetSamplers(first + low, high - low + 1, states + low);
        else context->PSSetSamplers(first + low, high - low + 1, states + low);
    }
    // count 0: the whole buffer (*SetConstantBuffers); otherwise a window.
    void setConstants(int stage, UINT reg, ID3D11Buffer* buffer, UINT first, UINT count) {
        if (reg >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) return;
        if (!change(bound.constants[stage][reg], D3D11Bound::Constants{buffer, first, count})) return;
        if (count) {
            if (stage == 0) context1->VSSetConstantBuffers1(reg, 1, &buffer, &first, &count);
            else if (stage == 1) context1->GSSetConstantBuffers1(reg, 1, &buffer, &first, &count);
            else context1->PSSetConstantBuffers1(reg, 1, &buffer, &first, &count);
        } else {
            if (stage == 0) context->VSSetConstantBuffers(reg, 1, &buffer);
            else if (stage == 1) context->GSSetConstantBuffers(reg, 1, &buffer);
            else context->PSSetConstantBuffers(reg, 1, &buffer);
        }
    }
    // Without constant buffer offsets: the window's constants, from the CPU
    // memory, into the register's dynamic buffer (renamed by the driver at
    // each WRITE_DISCARD, so each draw keeps what it was given), bound whole.
    void setWindowConstants(UINT reg, D3D11Buffer* buffer, UINT first, UINT count) {
        if (reg >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) return;
        const uint64_t begin = uint64_t(first) * 16, bytes = uint64_t(count) * 16;
        ID3D11Buffer* target = nullptr;
        if (buffer->cpu.empty()) {
            // A GPU buffer: whole, as without an offset (the renderer binds
            // windows of its upload ring only).
            static bool reported = false;
            if (begin && !reported) {
                reported = true;
                std::cerr << "NATIVE_D3D11 window_constants=gpu-buffer-at-offset (bound whole)\n";
            }
            target = buffer->constantBuffer();
        } else if ((target = device->windowConstantBuffer(reg, UINT(bytes)))) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(target, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
            const uint64_t available =
                begin < buffer->cpu.size() ? std::min<uint64_t>(bytes, buffer->cpu.size() - begin) : 0;
            std::memcpy(mapped.pData, buffer->cpu.data() + begin, size_t(available));
            const uint64_t size = device->windowConstantBytes[reg];
            if (available < size) std::memset(static_cast<uint8_t*>(mapped.pData) + available, 0, size_t(size - available));
            context->Unmap(target, 0);
            windowBytes += available;
        }
        if (!target) return;
        setConstants(0, reg, target, 0, 0);
        setConstants(2, reg, target, 0, 0);
    }
    void setVertexBuffer(UINT slot, ID3D11Buffer* buffer, UINT stride, UINT offset) {
        if (slot >= D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT) return;
        if (change(bound.vertices[slot], D3D11Bound::Vertices{buffer, stride, offset}))
            context->IASetVertexBuffers(slot, 1, &buffer, &stride, &offset);
    }
    void setIndexBuffer(ID3D11Buffer* buffer, DXGI_FORMAT format, UINT offset) {
        if (bound.indexBuffer == buffer && bound.indexFormat == format && bound.indexOffset == offset) {
            ++skipped;
            return;
        }
        ++calls;
        bound.indexBuffer = buffer;
        bound.indexFormat = format;
        bound.indexOffset = offset;
        context->IASetIndexBuffer(buffer, format, offset);
    }
    void setViewports(const D3D11_VIEWPORT* data, UINT count) {
        if (bound.viewportsKnown && viewports.size() == count && std::memcmp(viewports.data(), data, count * sizeof *data) == 0) {
            ++skipped;
            return;
        }
        ++calls;
        viewports.assign(data, data + count);
        bound.viewportsKnown = true;
        context->RSSetViewports(count, data);
    }
    void setScissors(const D3D11_RECT* data, UINT count) {
        if (bound.scissorsKnown && scissors.size() == count && std::memcmp(scissors.data(), data, count * sizeof *data) == 0) {
            ++skipped;
            return;
        }
        ++calls;
        scissors.assign(data, data + count);
        bound.scissorsKnown = true;
        context->RSSetScissorRects(count, data);
    }
    void setFramebuffer(const D3D11Framebuffer* target) {
        framebuffer = target;
        if (!change(bound.framebuffer, target)) return;
        // Binding targets unbinds the views of their textures that shaders
        // read (and a view of a bound target binds as null): what the
        // shaders see is no longer known.
        bound.forgetViews();
        if (!target) {
            context->OMSetRenderTargets(0, nullptr, nullptr);
            return;
        }
        std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> colors{};
        const UINT count = UINT(std::min(target->colors.size(), colors.size()));
        for (UINT i = 0; i < count; ++i) colors[i] = target->colors[i].Get();
        context->OMSetRenderTargets(count, colors.data(), target->depth.Get());
    }
    void run(const D3D11CommandList& list);
};

// D3D11Interface

D3D11Interface::D3D11Interface() {
    capabilities.shaderFormat = RenderShaderFormat::DXIL;
    if (const char* warp_setting = std::getenv("SFR_D3D11_WARP"); warp_setting && *warp_setting == '1') warp = true;
    if (const char* level = std::getenv("SFR_D3D11_FEATURE_LEVEL"); level && std::strcmp(level, "11_0") == 0)
        maxFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    if (const char* debug_setting = std::getenv("SFR_D3D11_DEBUG"); debug_setting && *debug_setting == '1') debug = true;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        char name[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof name - 1, nullptr, nullptr);
        deviceNames.push_back(name);
    }
}

std::unique_ptr<RenderDevice> D3D11Interface::createDevice(const std::string&) {
    ComPtr<IDXGIAdapter1> chosen;
    if (!warp && factory) {
        // The first hardware adapter (as DXGI orders them: the one driving the
        // main display), as D3D12's own selection.
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            chosen = adapter;
            break;
        }
    }
    auto device = std::make_unique<D3D11Device>(this, chosen);
    if (!device->valid()) return nullptr;
    return device;
}

std::unique_ptr<RenderInterface> CreateD3D11Interface() {
    auto renderInterface = std::make_unique<D3D11Interface>();
    if (!renderInterface->factory) return nullptr;
    return renderInterface;
}

// D3D11Device

D3D11Device::D3D11Device(D3D11Interface* renderInterface, ComPtr<IDXGIAdapter1> chosen) {
    this->renderInterface = renderInterface;
    adapter = chosen;
    if (const char* setting = std::getenv("SFR_D3D11_PREWARM"); setting && *setting == '0') prewarm = false;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const D3D_FEATURE_LEVEL* first = renderInterface->maxFeatureLevel == D3D_FEATURE_LEVEL_11_0 ? levels + 1 : levels;
    const UINT count = UINT(std::end(levels) - first);
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (renderInterface->debug) flags |= D3D11_CREATE_DEVICE_DEBUG;
    const D3D_DRIVER_TYPE type = adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP;
    HRESULT result = D3D11CreateDevice(adapter.Get(), type, nullptr, flags, first, count, D3D11_SDK_VERSION, &d3d,
                                       &featureLevel, &context);
    if (FAILED(result) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
        std::cerr << "NATIVE_D3D11 debug_layer=unavailable\n";
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        result = D3D11CreateDevice(adapter.Get(), type, nullptr, flags, first, count, D3D11_SDK_VERSION, &d3d,
                                   &featureLevel, &context);
    }
    if (FAILED(result)) {
        std::cerr << "NATIVE_D3D11 create=" << hresult(result) << '\n';
        d3d.Reset();
        return;
    }
    d3d.As(&d3d1);
    context.As(&context1);
    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    if (SUCCEEDED(d3d->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof options)))
        constantBufferOffsets = options.ConstantBufferOffsetting && context1;
    // SFR_D3D11_NO_CB_OFFSET=1: as a driver without constant buffer offsets
    // (the copying fallback), to test it on one that has them.
    if (const char* setting = std::getenv("SFR_D3D11_NO_CB_OFFSET"); setting && *setting == '1')
        constantBufferOffsets = false;
    std::cerr << "NATIVE_D3D11_OPTIONS offsetting=" << options.ConstantBufferOffsetting
              << " partial_update=" << options.ConstantBufferPartialUpdate
              << " map_default_buffer=" << options.MapNoOverwriteOnDynamicConstantBuffer << '\n';
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> used;
    if (SUCCEEDED(d3d.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&used))) {
        DXGI_ADAPTER_DESC desc{};
        used->GetDesc(&desc);
        char name[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof name - 1, nullptr, nullptr);
        description.name = name;
        description.vendor = RenderDeviceVendor(desc.VendorId);
        description.dedicatedVideoMemory = desc.DedicatedVideoMemory;
        LARGE_INTEGER version{};
        if (SUCCEEDED(used->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version)))
            description.driverVersion = uint64_t(version.QuadPart);
        description.type = adapter ? (desc.DedicatedVideoMemory < (512ull << 20) ? RenderDeviceType::INTEGRATED
                                                                                 : RenderDeviceType::DISCRETE)
                                   : RenderDeviceType::CPU;
    }
    capabilities.geometryShader = true;
    capabilities.maxTextureSize = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    std::cerr << "NATIVE_D3D11 feature_level=" << std::hex << unsigned(featureLevel) << std::dec
              << " warp=" << (adapter == nullptr) << " constant_buffer_offsets=" << constantBufferOffsets
              << " clear_view=" << (context1 != nullptr) << '\n';
    // The renderer's constants are ranges of its upload ring: without
    // offsets each draw's are copied into buffers of their own.
    if (!constantBufferOffsets) std::cerr << "NATIVE_D3D11 constant_windows=copied\n";

    // Clearing part of a depth target (clearDepthStencil with rectangles).
    static const char clear_source[] = R"(
cbuffer SfrClear : register(b13) { float4 sfrClearDepth; };
float4 main(uint index : SV_VertexID) : SV_Position {
    const float2 corner = float2((index << 1) & 2, index & 2);
    return float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), sfrClearDepth.x, 1.0);
}
)";
    std::string errors;
    const auto clear_code = compile(clear_source, "main", "vs_5_0", {}, &errors);
    if (clear_code.empty() || FAILED(d3d->CreateVertexShader(clear_code.data(), clear_code.size(), nullptr, &clearVertexShader)))
        std::cerr << "NATIVE_D3D11 clear_shader=unavailable " << errors << '\n';
    D3D11_BUFFER_DESC constants{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    d3d->CreateBuffer(&constants, nullptr, &clearConstants);
    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.ScissorEnable = TRUE;
    d3d->CreateRasterizerState(&rasterizer, &clearRasterizer);
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].BlendOp = blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = 0;
    d3d->CreateBlendState(&blend, &clearBlend);
}

ID3D11Buffer* D3D11Device::windowConstantBuffer(UINT reg, uint32_t bytes) {
    if (reg >= windowConstants.size()) return nullptr;
    bytes = (bytes + 15) & ~15u;
    if (!windowConstants[reg] || windowConstantBytes[reg] < bytes) {
        windowConstants[reg].Reset();
        windowConstantBytes[reg] = 0;
        D3D11_BUFFER_DESC desc{bytes, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
        if (const HRESULT result = d3d->CreateBuffer(&desc, nullptr, &windowConstants[reg]); FAILED(result)) {
            std::cerr << "NATIVE_D3D11 window_constants bytes=" << bytes << " create=" << hresult(result) << '\n';
            return nullptr;
        }
        windowConstantBytes[reg] = bytes;
    }
    return windowConstants[reg].Get();
}

std::unique_ptr<RenderDescriptorSet> D3D11Device::createDescriptorSet(const RenderDescriptorSetDesc& desc) {
    return std::make_unique<D3D11DescriptorSet>(desc);
}

std::unique_ptr<RenderShader> D3D11Device::createShader(const void* data, uint64_t size, const char* entryPointName,
                                                        RenderShaderFormat) {
    auto shader = std::make_unique<D3D11Shader>();
    const auto* bytes = static_cast<const uint8_t*>(data);
    shader->code.assign(bytes, bytes + size);
    shader->entryPoint = entryPointName ? entryPointName : "main";
    shader->source = size < 4 || std::memcmp(bytes, "DXBC", 4) != 0;
    if (shader->source && prewarm) prewarmShader(*shader);
    return shader;
}

std::vector<uint8_t> D3D11Device::shaderCode(const D3D11Shader* shader, const char* profile, uint32_t specConstants) {
    if (!shader->source) return shader->code;
    return compiledCode(shader->code, shader->entryPoint, profile, specConstants, false);
}

std::vector<uint8_t> D3D11Device::compiledCode(const std::vector<uint8_t>& source, const std::string& entry, const char* profile,
                                               uint32_t specConstants, bool prewarming) {
    const std::string key = D3D11ShaderKey(source, entry, profile, specConstants);
    // Compiled ahead of time (shaders-d3d11.pack): nothing to compile or wait for.
    if (D3D11PrecompiledShader)
        if (const auto* packed = D3D11PrecompiledShader(key)) {
            const uint32_t hits = ++precompiledHits;
            if (hits == 1 || hits % 100 == 0) std::cerr << "NATIVE_D3D11_SHADER packed_hits=" + std::to_string(hits) + '\n';
            return *packed;
        }
    std::promise<std::vector<uint8_t>> promise;
    {
        std::unique_lock guard(shaderMutex);
        if (const auto found = compiledShaders.find(key); found != compiledShaders.end()) {
            // Done, or being compiled on another thread: its result.
            auto future = found->second;
            guard.unlock();
            return future.get();
        }
        compiledShaders.emplace(key, promise.get_future().share());
    }
    const auto folder = shaderCacheFolder();
    const auto file = folder / (key + ".dxbc");
    std::vector<uint8_t> code;
    if (std::ifstream in{file, std::ios::binary}) code.assign(std::istreambuf_iterator<char>(in), {});
    if (code.size() < 4 || std::memcmp(code.data(), "DXBC", 4) != 0) {
        std::string errors;
        const auto started = std::chrono::steady_clock::now();
        code = D3D11CompileShader(source, entry, profile, specConstants, &errors);
        const double milliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (code.empty()) {
            for (char& c : errors) if (c == '\n' || c == '\r') c = ' ';
            std::cerr << "NATIVE_D3D11_SHADER compiled=0 key=" + std::string(key) + " errors=" + errors.substr(0, 600) + '\n';
        } else {
            std::cerr << "NATIVE_D3D11_SHADER compiled=1 key=" + std::string(key) + " ms=" + std::to_string(int(milliseconds)) +
                             " prewarm=" + (prewarming ? "1" : "0") + '\n';
            // Written beside and renamed, so that no reader sees half a file.
            std::error_code error;
            std::filesystem::create_directories(folder, error);
            auto partial = file;
            partial += ".part" + std::to_string(GetCurrentThreadId());
            {
                std::ofstream out(partial, std::ios::binary);
                out.write(reinterpret_cast<const char*>(code.data()), std::streamsize(code.size()));
            }
            std::filesystem::rename(partial, file, error);
            if (error) std::filesystem::remove(partial, error);
        }
    }
    promise.set_value(code);
    return code;
}

void D3D11Device::prewarmShader(const D3D11Shader& shader) {
    // Which stage the translated HLSL is for: only pixel shaders write
    // SV_Target; vertex shaders read the VertexShaderInput.
    const std::string_view text(reinterpret_cast<const char*>(shader.code.data()), shader.code.size());
    const char* profile = text.find("SV_Target") != std::string_view::npos           ? "ps_5_0"
                          : text.find("VertexShaderInput") != std::string_view::npos ? "vs_5_0"
                                                                                     : nullptr;
    if (!profile) return;
    std::lock_guard guard(prewarmMutex);
    if (prewarmThreads.empty()) {
        // Leave the game's own threads a core or two (a dual-core laptop
        // gets one compiler thread).
        const unsigned cores = std::max(std::thread::hardware_concurrency(), 1u);
        const unsigned count = std::clamp(cores > 2 ? cores - 2 : 1u, 1u, 4u);
        for (unsigned i = 0; i < count; ++i)
            prewarmThreads.emplace_back([this] {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                for (;;) {
                    std::function<void()> job;
                    {
                        std::unique_lock lock(prewarmMutex);
                        prewarmWake.wait(lock, [this] { return prewarmStop || !prewarmJobs.empty(); });
                        if (prewarmStop) return;
                        job = std::move(prewarmJobs.front());
                        prewarmJobs.pop_front();
                    }
                    job();
                }
            });
    }
    prewarmJobs.push_back([this, source = shader.code, entry = shader.entryPoint, profile] {
        compiledCode(source, entry, profile, 0, true);
    });
    prewarmWake.notify_one();
}

D3D11Device::~D3D11Device() {
    {
        std::lock_guard guard(prewarmMutex);
        prewarmStop = true;
        prewarmJobs.clear();
    }
    prewarmWake.notify_all();
    for (auto& thread : prewarmThreads) thread.join();
}

std::unique_ptr<RenderSampler> D3D11Device::createSampler(const RenderSamplerDesc& desc) {
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = toFilter(desc);
    sampler.AddressU = cast_or(desc.addressU, D3D11_TEXTURE_ADDRESS_WRAP);
    sampler.AddressV = cast_or(desc.addressV, D3D11_TEXTURE_ADDRESS_WRAP);
    sampler.AddressW = cast_or(desc.addressW, D3D11_TEXTURE_ADDRESS_WRAP);
    sampler.MipLODBias = desc.mipLODBias;
    sampler.MaxAnisotropy = desc.anisotropyEnabled ? std::clamp(desc.maxAnisotropy, 1u, 16u) : 1u;
    sampler.ComparisonFunc = cast_or(desc.comparisonFunc, D3D11_COMPARISON_NEVER);
    const float border = desc.borderColor == RenderBorderColor::OPAQUE_WHITE ? 1.0f : 0.0f;
    const float alpha = desc.borderColor == RenderBorderColor::TRANSPARENT_BLACK ? 0.0f : 1.0f;
    sampler.BorderColor[0] = sampler.BorderColor[1] = sampler.BorderColor[2] = border;
    sampler.BorderColor[3] = alpha;
    sampler.MinLOD = desc.minLOD;
    sampler.MaxLOD = desc.maxLOD;
    auto result = std::make_unique<D3D11Sampler>();
    if (FAILED(d3d->CreateSamplerState(&sampler, &result->d3d))) return nullptr;
    return result;
}

std::unique_ptr<RenderPipeline> D3D11Device::createGraphicsPipeline(const RenderGraphicsPipelineDesc& desc) {
    auto pipeline = std::make_unique<D3D11GraphicsPipeline>();
    const uint32_t spec = desc.specConstantsCount ? desc.specConstants[0].value : 0;
    const auto* vertex = static_cast<const D3D11Shader*>(desc.vertexShader);
    const auto* geometry = static_cast<const D3D11Shader*>(desc.geometryShader);
    const auto* pixel = static_cast<const D3D11Shader*>(desc.pixelShader);
    if (!vertex) return pipeline;
    const auto vertexCode = shaderCode(vertex, "vs_5_0", 0);
    if (vertexCode.empty() || FAILED(d3d->CreateVertexShader(vertexCode.data(), vertexCode.size(), nullptr, &pipeline->vs)))
        return pipeline;
    if (geometry) {
        const auto code = shaderCode(geometry, "gs_5_0", 0);
        if (code.empty() || FAILED(d3d->CreateGeometryShader(code.data(), code.size(), nullptr, &pipeline->gs))) return pipeline;
    }
    if (pixel) {
        const auto code = shaderCode(pixel, "ps_5_0", spec);
        if (code.empty() || FAILED(d3d->CreatePixelShader(code.data(), code.size(), nullptr, &pipeline->ps))) return pipeline;
    }
    if (desc.inputElementsCount) {
        std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
        for (uint32_t i = 0; i < desc.inputElementsCount; ++i) {
            const auto& element = desc.inputElements[i];
            D3D11_INPUT_ELEMENT_DESC out{};
            out.SemanticName = element.semanticName;
            out.SemanticIndex = element.semanticIndex;
            out.Format = toDXGI(element.format);
            out.InputSlot = element.slotIndex;
            out.AlignedByteOffset = element.alignedByteOffset;
            out.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            for (uint32_t j = 0; j < desc.inputSlotsCount; ++j)
                if (desc.inputSlots[j].index == element.slotIndex &&
                    desc.inputSlots[j].classification == RenderInputSlotClassification::PER_INSTANCE_DATA) {
                    out.InputSlotClass = D3D11_INPUT_PER_INSTANCE_DATA;
                    out.InstanceDataStepRate = 1;
                }
            elements.push_back(out);
        }
        if (const HRESULT result = d3d->CreateInputLayout(elements.data(), UINT(elements.size()), vertexCode.data(),
                                                          vertexCode.size(), &pipeline->inputLayout);
            FAILED(result)) {
            std::cerr << "NATIVE_D3D11 input_layout=" << hresult(result) << '\n';
            return pipeline;
        }
    }
    pipeline->inputSlots.assign(desc.inputSlots, desc.inputSlots + desc.inputSlotsCount);
    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = cast_or(desc.cullMode, D3D11_CULL_NONE);
    rasterizer.DepthBias = desc.depthBias;
    rasterizer.DepthBiasClamp = desc.depthBiasClamp;
    rasterizer.SlopeScaledDepthBias = desc.slopeScaledDepthBias;
    rasterizer.DepthClipEnable = desc.depthClipEnabled;
    rasterizer.ScissorEnable = TRUE;  // D3D12 and Vulkan always test the scissor
    if (FAILED(d3d->CreateRasterizerState(&rasterizer, &pipeline->rasterizer))) return pipeline;
    D3D11_BLEND_DESC blend{};
    blend.AlphaToCoverageEnable = desc.alphaToCoverageEnabled;
    blend.IndependentBlendEnable = desc.renderTargetCount > 1;
    for (uint32_t i = 0; i < 8; ++i) {
        auto& target = blend.RenderTarget[i];
        const auto& source = desc.renderTargetBlend[i < desc.renderTargetCount ? i : 0];
        target.BlendEnable = i < desc.renderTargetCount && source.blendEnabled;
        target.SrcBlend = toD3D11(source.srcBlend, D3D11_BLEND_ONE);
        target.DestBlend = toD3D11(source.dstBlend, D3D11_BLEND_ZERO);
        target.BlendOp = cast_or(source.blendOp, D3D11_BLEND_OP_ADD);
        target.SrcBlendAlpha = toD3D11(source.srcBlendAlpha, D3D11_BLEND_ONE);
        target.DestBlendAlpha = toD3D11(source.dstBlendAlpha, D3D11_BLEND_ZERO);
        target.BlendOpAlpha = cast_or(source.blendOpAlpha, D3D11_BLEND_OP_ADD);
        target.RenderTargetWriteMask = i < desc.renderTargetCount ? source.renderTargetWriteMask : D3D11_COLOR_WRITE_ENABLE_ALL;
    }
    if (FAILED(d3d->CreateBlendState(&blend, &pipeline->blend))) return pipeline;
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = desc.depthEnabled;
    depth.DepthWriteMask = desc.depthWriteEnabled ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = cast_or(desc.depthFunction, D3D11_COMPARISON_ALWAYS);
    depth.StencilEnable = desc.stencilEnabled;
    depth.StencilReadMask = UINT8(desc.stencilReadMask);
    depth.StencilWriteMask = UINT8(desc.stencilWriteMask);
    const auto face = [](const RenderStencilFaceDesc& in) {
        D3D11_DEPTH_STENCILOP_DESC out{};
        out.StencilFailOp = cast_or(in.failOp, D3D11_STENCIL_OP_KEEP);
        out.StencilDepthFailOp = cast_or(in.depthFailOp, D3D11_STENCIL_OP_KEEP);
        out.StencilPassOp = cast_or(in.passOp, D3D11_STENCIL_OP_KEEP);
        out.StencilFunc = cast_or(in.compareFunction, D3D11_COMPARISON_ALWAYS);
        return out;
    };
    depth.FrontFace = face(desc.stencilFrontFace);
    depth.BackFace = face(desc.stencilBackFace);
    if (FAILED(d3d->CreateDepthStencilState(&depth, &pipeline->depthStencil))) return pipeline;
    pipeline->topology = toTopology(desc.primitiveTopology);
    pipeline->stencilReference = desc.stencilReference;
    pipeline->valid = true;
    return pipeline;
}

std::unique_ptr<RenderCommandQueue> D3D11Device::createCommandQueue(RenderCommandListType) {
    return std::make_unique<D3D11CommandQueue>(this);
}

std::unique_ptr<RenderBuffer> D3D11Device::createBuffer(const RenderBufferDesc& desc) {
    auto buffer = std::make_unique<D3D11Buffer>(this, desc);
    const bool host = desc.heapType == RenderHeapType::UPLOAD || desc.heapType == RenderHeapType::READBACK;
    if (!host && !buffer->gpu && !buffer->constants) return nullptr;
    return buffer;
}

std::unique_ptr<RenderTexture> D3D11Device::createTexture(const RenderTextureDesc& desc) {
    auto texture = std::make_unique<D3D11Texture>(this, desc);
    if (!texture->d3d) return nullptr;
    return texture;
}

namespace {
struct D3D11Pool : RenderPool {
    D3D11Device* device;
    explicit D3D11Pool(D3D11Device* device) : device(device) {}
    std::unique_ptr<RenderBuffer> createBuffer(const RenderBufferDesc& desc) override { return device->createBuffer(desc); }
    std::unique_ptr<RenderTexture> createTexture(const RenderTextureDesc& desc) override { return device->createTexture(desc); }
};
}

std::unique_ptr<RenderPool> D3D11Device::createPool(const RenderPoolDesc&) { return std::make_unique<D3D11Pool>(this); }

std::unique_ptr<RenderPipelineLayout> D3D11Device::createPipelineLayout(const RenderPipelineLayoutDesc& desc) {
    auto layout = std::make_unique<D3D11PipelineLayout>();
    layout->pushConstantRanges.assign(desc.pushConstantRanges, desc.pushConstantRanges + desc.pushConstantRangesCount);
    layout->rootDescriptors.assign(desc.rootDescriptorDescs, desc.rootDescriptorDescs + desc.rootDescriptorDescsCount);
    for (const auto& range : layout->pushConstantRanges) {
        D3D11_BUFFER_DESC constants{(range.size + 15u) & ~15u, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        ComPtr<ID3D11Buffer> buffer;
        if (FAILED(d3d->CreateBuffer(&constants, nullptr, &buffer))) return nullptr;
        layout->pushConstantBuffers.push_back(buffer);
    }
    return layout;
}

std::unique_ptr<RenderCommandFence> D3D11Device::createCommandFence() {
    auto fence = std::make_unique<D3D11CommandFence>();
    D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
    if (FAILED(d3d->CreateQuery(&query, &fence->query))) return nullptr;
    return fence;
}

std::unique_ptr<RenderCommandSemaphore> D3D11Device::createCommandSemaphore() {
    return std::make_unique<D3D11CommandSemaphore>();
}

std::unique_ptr<RenderFramebuffer> D3D11Device::createFramebuffer(const RenderFramebufferDesc& desc) {
    auto framebuffer = std::make_unique<D3D11Framebuffer>();
    for (uint32_t i = 0; i < desc.colorAttachmentsCount; ++i) {
        const auto* texture = static_cast<const D3D11Texture*>(desc.colorAttachments[i]);
        D3D11_RENDER_TARGET_VIEW_DESC view{};
        view.Format = toDXGI(texture->desc.format);
        view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11RenderTargetView> target;
        if (FAILED(d3d->CreateRenderTargetView(texture->d3d.Get(), &view, &target))) return nullptr;
        framebuffer->colors.push_back(target);
        framebuffer->width = texture->desc.width;
        framebuffer->height = texture->desc.height;
    }
    if (desc.depthAttachment) {
        const auto* texture = static_cast<const D3D11Texture*>(desc.depthAttachment);
        D3D11_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = toDepthView(texture->desc.format);
        view.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        if (desc.depthAttachmentReadOnly) view.Flags = D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL;
        if (FAILED(d3d->CreateDepthStencilView(texture->d3d.Get(), &view, &framebuffer->depth))) return nullptr;
        framebuffer->width = texture->desc.width;
        framebuffer->height = texture->desc.height;
    }
    return framebuffer;
}

// D3D11Buffer

D3D11Buffer::D3D11Buffer(D3D11Device* device, const RenderBufferDesc& desc) : device(device), desc(desc) {
    const bool upload = desc.heapType == RenderHeapType::UPLOAD || desc.heapType == RenderHeapType::GPU_UPLOAD;
    if (upload || desc.heapType == RenderHeapType::READBACK) cpu.assign(size_t(desc.size), 0);
    if (desc.heapType == RenderHeapType::READBACK) return;
    const UINT size = UINT((desc.size + 15) & ~uint64_t(15));
    UINT bind = 0;
    if (desc.flags & RenderBufferFlag::VERTEX) bind |= D3D11_BIND_VERTEX_BUFFER;
    if (desc.flags & RenderBufferFlag::INDEX) bind |= D3D11_BIND_INDEX_BUFFER;
    UINT misc = 0;
    if (desc.flags & (RenderBufferFlag::STORAGE | RenderBufferFlag::UNORDERED_ACCESS)) {
        bind |= D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        misc |= D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    }
    // An upload buffer always has a GPU copy: its constants copy from it.
    if (upload && !bind) bind = D3D11_BIND_VERTEX_BUFFER;
    if (bind || !upload) {
        D3D11_BUFFER_DESC buffer{size, D3D11_USAGE_DEFAULT, bind, 0, misc, 0};
        if (FAILED(device->d3d->CreateBuffer(&buffer, nullptr, &gpu))) gpu.Reset();
    }
    if (!upload && (desc.flags & RenderBufferFlag::CONSTANT)) constantBuffer();
}

ID3D11Buffer* D3D11Buffer::constantBuffer() {
    if (!constants) {
        D3D11_BUFFER_DESC buffer{UINT((desc.size + 255) & ~uint64_t(255)), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        if (const HRESULT result = device->d3d->CreateBuffer(&buffer, nullptr, &constants); FAILED(result))
            std::cerr << "NATIVE_D3D11 constant_buffer bytes=" << buffer.ByteWidth << " create=" << hresult(result) << '\n';
    }
    return constants.Get();
}

void* D3D11Buffer::map(uint32_t, const RenderRange*) {
    if (cpu.empty()) return nullptr;
    ++mapCount;
    dirty = true;
    return cpu.data();
}

void D3D11Buffer::unmap(uint32_t, const RenderRange*) {
    if (mapCount) --mapCount;
}

std::unique_ptr<RenderBufferFormattedView> D3D11Buffer::createBufferFormattedView(RenderFormat) {
    return std::make_unique<RenderBufferFormattedView>();
}

// D3D11Texture

D3D11Texture::D3D11Texture(D3D11Device* device, const RenderTextureDesc& desc) : device(device), desc(desc) {
    if (desc.dimension != RenderTextureDimension::TEXTURE_2D) {
        std::cerr << "NATIVE_D3D11 texture_dimension=" << int(desc.dimension) << " unsupported\n";
        return;
    }
    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = desc.width;
    texture.Height = desc.height;
    texture.MipLevels = std::max(desc.mipLevels, 1u);
    texture.ArraySize = std::max(desc.arraySize, 1u);
    texture.Format = toDXGI(desc.format);
    texture.SampleDesc.Count = std::max(desc.multisampling.sampleCount, 1u);
    texture.Usage = D3D11_USAGE_DEFAULT;
    texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (desc.flags & RenderTextureFlag::RENDER_TARGET) texture.BindFlags |= D3D11_BIND_RENDER_TARGET;
    if (desc.flags & RenderTextureFlag::DEPTH_TARGET) texture.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
    if (desc.flags & RenderTextureFlag::UNORDERED_ACCESS) texture.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    const bool cube = (desc.flags & RenderTextureFlag::CUBE) && texture.ArraySize % 6 == 0;
    if (cube) texture.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    if (const HRESULT result = device->d3d->CreateTexture2D(&texture, nullptr, &d3d); FAILED(result)) {
        std::cerr << "NATIVE_D3D11 texture format=" << int(desc.format) << " size=" << desc.width << 'x' << desc.height
                  << " create=" << hresult(result) << '\n';
        d3d.Reset();
        return;
    }
    RenderTextureViewDesc view;
    view.format = desc.format;
    view.dimension = cube ? RenderTextureViewDimension::TEXTURE_CUBE : RenderTextureViewDimension::TEXTURE_2D;
    auto made = createTextureView(view);
    if (made) srv = static_cast<D3D11TextureView*>(made.get())->srv;
}

std::unique_ptr<RenderTextureView> D3D11Texture::createTextureView(const RenderTextureViewDesc& view) const {
    if (!d3d) return nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = toShaderView(view.format == RenderFormat::UNKNOWN ? desc.format : view.format);
    const uint32_t layers = std::max(desc.arraySize, 1u);
    const uint32_t mips = std::max(desc.mipLevels, 1u);
    const uint32_t viewMips = view.mipLevels == UINT32_MAX ? mips - view.mipSlice : view.mipLevels;
    if (view.dimension == RenderTextureViewDimension::TEXTURE_CUBE) {
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        srvDesc.TextureCube.MostDetailedMip = view.mipSlice;
        srvDesc.TextureCube.MipLevels = viewMips;
    } else if (desc.multisampling.sampleCount > 1) {
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
    } else if (layers > 1) {
        // As Plume's D3D12 backend: a 2D view of more than one layer is an array view.
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Texture2DArray.MostDetailedMip = view.mipSlice;
        srvDesc.Texture2DArray.MipLevels = viewMips;
        srvDesc.Texture2DArray.FirstArraySlice = view.arrayIndex;
        srvDesc.Texture2DArray.ArraySize = view.arraySize == UINT32_MAX ? layers - view.arrayIndex : view.arraySize;
    } else {
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = view.mipSlice;
        srvDesc.Texture2D.MipLevels = viewMips;
    }
    // D3D11 views have no component mapping. The renderer's only swizzle is
    // all ZERO (its null textures, R8 with alpha read as zero too): a view of
    // an RGBA8 texture of the same shape holding zeros instead.
    const auto& mapping = view.componentMapping;
    ComPtr<ID3D11Texture2D> zeros;
    if (mapping.r == RenderSwizzle::ZERO && mapping.g == RenderSwizzle::ZERO && mapping.b == RenderSwizzle::ZERO &&
        mapping.a == RenderSwizzle::ZERO) {
        D3D11_TEXTURE2D_DESC zero{};
        d3d->GetDesc(&zero);
        zero.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        zero.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const std::vector<uint32_t> texels(size_t(zero.Width) * zero.Height, 0);
        std::vector<D3D11_SUBRESOURCE_DATA> initial(size_t(zero.MipLevels) * zero.ArraySize);
        for (auto& data : initial) data = {texels.data(), zero.Width * 4, zero.Width * zero.Height * 4};
        if (FAILED(device->d3d->CreateTexture2D(&zero, initial.data(), &zeros))) return nullptr;
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    auto result = std::make_unique<D3D11TextureView>();
    if (FAILED(device->d3d->CreateShaderResourceView(zeros ? zeros.Get() : d3d.Get(), &srvDesc, &result->srv))) return nullptr;
    return result;
}

// D3D11DescriptorSet

D3D11DescriptorSet::D3D11DescriptorSet(const RenderDescriptorSetDesc& desc) {
    for (uint32_t r = 0; r < desc.descriptorRangesCount; ++r) {
        const auto& range = desc.descriptorRanges[r];
        const bool last = r + 1 == desc.descriptorRangesCount;
        const uint32_t count = last && desc.lastRangeIsBoundless ? std::max(desc.boundlessRangeSize, range.count) : range.count;
        for (uint32_t i = 0; i < count; ++i) {
            Entry entry;
            entry.type = range.type;
            entry.reg = range.binding + i;
            entries.push_back(entry);
        }
    }
}

void D3D11DescriptorSet::setBuffer(uint32_t index, const RenderBuffer* buffer, uint64_t, const RenderBufferStructuredView*,
                                   const RenderBufferFormattedView*) {
    if (index >= entries.size()) return;
    entries[index].buffer = const_cast<D3D11Buffer*>(static_cast<const D3D11Buffer*>(buffer));
}

void D3D11DescriptorSet::setTexture(uint32_t index, const RenderTexture* texture, RenderTextureLayout,
                                    const RenderTextureView* view) {
    if (index >= entries.size()) return;
    if (view) entries[index].srv = static_cast<const D3D11TextureView*>(view)->srv;
    else if (texture) entries[index].srv = static_cast<const D3D11Texture*>(texture)->srv;
    else entries[index].srv.Reset();
}

void D3D11DescriptorSet::setSampler(uint32_t index, const RenderSampler* sampler) {
    if (index >= entries.size()) return;
    if (sampler) entries[index].sampler = static_cast<const D3D11Sampler*>(sampler)->d3d;
    else entries[index].sampler.Reset();
}

// D3D11CommandList

void D3D11CommandList::begin() {
    commands.clear();
    bytes.clear();
    slots.clear();
    vertexBindings.clear();
    viewportData.clear();
    scissorData.clear();
    callbacks.clear();
    uses.clear();
    recordingLayout = nullptr;
}

void D3D11CommandList::callback(std::function<void(D3D11Replay&)> function) {
    D3D11Command command{D3D11Op::callback};
    command.a = uint32_t(callbacks.size());
    callbacks.push_back(std::move(function));
    commands.push_back(command);
}

void D3D11CommandList::drawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance) {
    commands.push_back({D3D11Op::draw, vertexCount, instanceCount, startVertex, startInstance});
}

void D3D11CommandList::drawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t startIndex,
                                            int32_t baseVertex, uint32_t startInstance) {
    commands.push_back({D3D11Op::drawIndexed, indexCount, instanceCount, startIndex, startInstance, baseVertex});
}

void D3D11CommandList::setPipeline(const RenderPipeline* pipeline) {
    D3D11Command command{D3D11Op::pipeline};
    command.p = pipeline;
    commands.push_back(command);
}

void D3D11CommandList::setStencilReference(uint32_t reference) {
    commands.push_back({D3D11Op::stencilReference, reference});
}

void D3D11CommandList::setGraphicsPipelineLayout(const RenderPipelineLayout* pipelineLayout) {
    recordingLayout = static_cast<const D3D11PipelineLayout*>(pipelineLayout);
}

void D3D11CommandList::setGraphicsPushConstants(uint32_t rangeIndex, const void* data, uint32_t offset, uint32_t size) {
    const auto* layout = recordingLayout;
    if (!layout || rangeIndex >= layout->pushConstantRanges.size()) return;
    const auto& range = layout->pushConstantRanges[rangeIndex];
    if (!size) size = range.size;
    // Constant buffers update whole (D3D11.1's partial updates aside): the
    // whole buffer's bytes, zero where the range was not given.
    const uint32_t whole = (range.size + 15u) & ~15u;
    if (offset >= whole) return;
    D3D11Command command{D3D11Op::pushConstants, range.binding, uint32_t(bytes.size()), whole};
    command.p = layout->pushConstantBuffers[rangeIndex].Get();
    bytes.resize(bytes.size() + whole, 0);
    std::memcpy(bytes.data() + command.b + offset, data, std::min<size_t>(size, whole - offset));
    commands.push_back(command);
}

void D3D11CommandList::setGraphicsDescriptorSet(RenderDescriptorSet* descriptorSet, uint32_t) {
    D3D11Command command{D3D11Op::descriptorSet};
    command.p = descriptorSet;
    commands.push_back(command);
}

void D3D11CommandList::setXenosTextures(const RenderDescriptorSet* textures, const RenderDescriptorSet* samplers,
                                        const std::array<uint32_t, 64>& indices) {
    // The views themselves are read as the list replays, as D3D12 reads a
    // descriptor heap when the GPU runs.
    D3D11Command command{D3D11Op::xenosTextures, uint32_t(slots.size())};
    command.p = textures;
    command.q = samplers;
    for (const uint32_t index : indices) slots.push_back(uint16_t(std::min(index, 0xFFFFu)));
    commands.push_back(command);
}

void D3D11CommandList::setGraphicsRootDescriptor(RenderBufferReference reference, uint32_t rootDescriptorIndex) {
    const auto* layout = recordingLayout;
    if (!layout || rootDescriptorIndex >= layout->rootDescriptors.size() || !reference.ref) return;
    auto* buffer = const_cast<D3D11Buffer*>(static_cast<const D3D11Buffer*>(reference.ref));
    // A window of up to 4096 constants at the offset, in whole 16-constant
    // (256-byte) steps, as D3D11.1 binds them.
    const uint64_t offset = reference.offset & ~uint64_t(255);
    const uint64_t available = buffer->desc.size > offset ? buffer->desc.size - offset : 0;
    const UINT first = UINT(offset / 16);
    UINT count = UINT(std::min<uint64_t>(4096, (available / 16) & ~uint64_t(15)));
    if (!count) return;
    const bool offsets = queue->device->constantBufferOffsets;
    if (!offsets) {
        // Copied at the draw from the CPU memory: only what the shaders read.
        if (rootDescriptorIndex < layout->rootDescriptorBytes.size() && layout->rootDescriptorBytes[rootDescriptorIndex])
            count = std::min<UINT>(count, (layout->rootDescriptorBytes[rootDescriptorIndex] + 15) / 16);
    } else if (!buffer->cpu.empty()) {
        uses.push_back({buffer, offset, offset + uint64_t(count) * 16, true});
    }
    D3D11Command command{D3D11Op::rootDescriptor, layout->rootDescriptors[rootDescriptorIndex].shaderRegister, first, count};
    command.p = buffer;
    command.d = offsets ? 0 : 1;
    commands.push_back(command);
}

void D3D11CommandList::setIndexBuffer(const RenderIndexBufferView* view) {
    D3D11Command command{D3D11Op::indexBuffer, 0, DXGI_FORMAT_R32_UINT};
    if (view && view->buffer.ref) {
        auto* buffer = const_cast<D3D11Buffer*>(static_cast<const D3D11Buffer*>(view->buffer.ref));
        command.p = buffer;
        command.a = UINT(view->buffer.offset);
        command.b = toDXGI(view->format);
        if (!buffer->cpu.empty()) uses.push_back({buffer, command.a, command.a + uint64_t(view->size), false});
    }
    commands.push_back(command);
}

void D3D11CommandList::setVertexBuffers(uint32_t startSlot, const RenderVertexBufferView* views, uint32_t viewCount,
                                        const RenderInputSlot* inputSlots) {
    D3D11Command command{D3D11Op::vertexBuffers, startSlot, viewCount, uint32_t(vertexBindings.size())};
    for (uint32_t i = 0; i < viewCount; ++i) {
        auto* buffer = const_cast<D3D11Buffer*>(static_cast<const D3D11Buffer*>(views[i].buffer.ref));
        UINT stride = 0;
        for (uint32_t j = 0; inputSlots && j < viewCount; ++j)
            if (inputSlots[j].index == startSlot + i) stride = inputSlots[j].stride;
        vertexBindings.push_back({buffer, stride, UINT(views[i].buffer.offset)});
        if (buffer && !buffer->cpu.empty() && views[i].size)
            uses.push_back({buffer, views[i].buffer.offset, views[i].buffer.offset + views[i].size, false});
    }
    commands.push_back(command);
}

void D3D11CommandList::setViewports(const RenderViewport* viewports, uint32_t count) {
    commands.push_back({D3D11Op::viewports, uint32_t(viewportData.size()), count});
    for (uint32_t i = 0; i < count; ++i)
        viewportData.push_back({viewports[i].x, viewports[i].y, viewports[i].width, viewports[i].height,
                                viewports[i].minDepth, viewports[i].maxDepth});
}

void D3D11CommandList::setScissors(const RenderRect* rects, uint32_t count) {
    commands.push_back({D3D11Op::scissors, uint32_t(scissorData.size()), count});
    for (uint32_t i = 0; i < count; ++i) scissorData.push_back({rects[i].left, rects[i].top, rects[i].right, rects[i].bottom});
}

void D3D11CommandList::setFramebuffer(const RenderFramebuffer* framebuffer) {
    D3D11Command command{D3D11Op::framebuffer};
    command.p = framebuffer;
    commands.push_back(command);
}

void D3D11Replay::run(const D3D11CommandList& list) {
    for (const auto& command : list.commands) {
        switch (command.op) {
        case D3D11Op::draw:
            if (!pipeline || !pipeline->valid) break;
            draw("vertices", command.a, [&] {
                if (command.b == 1 && command.d == 0) context->Draw(command.a, command.c);
                else context->DrawInstanced(command.a, command.b, command.c, command.d);
            });
            break;
        case D3D11Op::drawIndexed:
            if (!pipeline || !pipeline->valid) break;
            draw("indices", command.a, [&] {
                if (command.b == 1 && command.d == 0) context->DrawIndexed(command.a, command.c, command.e);
                else context->DrawIndexedInstanced(command.a, command.b, command.c, command.e, command.d);
            });
            break;
        case D3D11Op::pipeline:
            pipeline = static_cast<const D3D11GraphicsPipeline*>(command.p);
            stencilOverride = false;
            applyPipeline();
            break;
        case D3D11Op::stencilReference:
            stencilReference = command.a;
            stencilOverride = true;
            applyPipeline();
            break;
        case D3D11Op::pushConstants: {
            auto* buffer = static_cast<ID3D11Buffer*>(const_cast<void*>(command.p));
            context->UpdateSubresource(buffer, 0, nullptr, list.bytes.data() + command.b, 0, 0);
            for (int stage = 0; stage < 3; ++stage) setConstants(stage, command.a, buffer, 0, 0);
            break;
        }
        case D3D11Op::descriptorSet: {
            const auto* set = static_cast<const D3D11DescriptorSet*>(command.p);
            for (const auto& entry : set->entries) {
                if (entry.type == RenderDescriptorRangeType::TEXTURE && entry.reg < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
                    ID3D11ShaderResourceView* view = entry.srv.Get();
                    setViews(0, entry.reg, 1, &view);
                    setViews(1, entry.reg, 1, &view);
                } else if (entry.type == RenderDescriptorRangeType::SAMPLER && entry.reg < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
                    ID3D11SamplerState* sampler = entry.sampler.Get();
                    setSamplers(0, entry.reg, 1, &sampler);
                    setSamplers(1, entry.reg, 1, &sampler);
                }
            }
            break;
        }
        case D3D11Op::xenosTextures: {
            const auto* textureSet = static_cast<const D3D11DescriptorSet*>(command.p);
            const auto* samplerSet = static_cast<const D3D11DescriptorSet*>(command.q);
            const uint16_t* indices = list.slots.data() + command.a;
            std::array<ID3D11ShaderResourceView*, 48> views;
            std::array<ID3D11SamplerState*, 16> states;
            for (uint32_t i = 0; i < 48; ++i)
                views[i] = indices[i] < textureSet->entries.size() ? textureSet->entries[indices[i]].srv.Get() : nullptr;
            for (uint32_t i = 0; i < 16; ++i)
                states[i] = indices[48 + i] < samplerSet->entries.size() ? samplerSet->entries[indices[48 + i]].sampler.Get()
                                                                         : nullptr;
            setViews(0, 0, 48, views.data());
            setViews(1, 0, 48, views.data());
            setSamplers(0, 0, 16, states.data());
            setSamplers(1, 0, 16, states.data());
            break;
        }
        case D3D11Op::rootDescriptor: {
            auto* buffer = static_cast<D3D11Buffer*>(const_cast<void*>(command.p));
            if (command.d) {
                setWindowConstants(command.a, buffer, command.b, command.c);
                break;
            }
            ID3D11Buffer* constants = buffer->constantBuffer();
            setConstants(0, command.a, constants, command.b, command.c);
            setConstants(2, command.a, constants, command.b, command.c);
            break;
        }
        case D3D11Op::indexBuffer: {
            const auto* buffer = static_cast<const D3D11Buffer*>(command.p);
            setIndexBuffer(buffer ? buffer->gpu.Get() : nullptr, DXGI_FORMAT(command.b), command.a);
            break;
        }
        case D3D11Op::vertexBuffers:
            for (uint32_t i = 0; i < command.b; ++i) {
                const auto& binding = list.vertexBindings[command.c + i];
                setVertexBuffer(command.a + i, binding.buffer ? binding.buffer->gpu.Get() : nullptr, binding.stride,
                                binding.offset);
            }
            break;
        case D3D11Op::viewports:
            setViewports(list.viewportData.data() + command.a, command.b);
            break;
        case D3D11Op::scissors:
            setScissors(list.scissorData.data() + command.a, command.b);
            break;
        case D3D11Op::framebuffer:
            setFramebuffer(static_cast<const D3D11Framebuffer*>(command.p));
            break;
        case D3D11Op::callback:
            list.callbacks[command.a](*this);
            break;
        }
    }
}

void D3D11CommandList::clearColor(uint32_t attachmentIndex, RenderColor color, const RenderRect* rects, uint32_t rectCount) {
    std::vector<D3D11_RECT> out(rectCount);
    for (uint32_t i = 0; i < rectCount; ++i) out[i] = {rects[i].left, rects[i].top, rects[i].right, rects[i].bottom};
    callback([attachmentIndex, color, out = std::move(out)](D3D11Replay& replay) {
        if (!replay.framebuffer || attachmentIndex >= replay.framebuffer->colors.size()) return;
        ID3D11RenderTargetView* target = replay.framebuffer->colors[attachmentIndex].Get();
        if (out.empty()) replay.context->ClearRenderTargetView(target, color.rgba);
        else replay.context1->ClearView(target, color.rgba, out.data(), UINT(out.size()));
    });
}

void D3D11CommandList::clearDepthStencil(bool clearDepth, bool clearStencil, float depthValue, uint32_t stencilValue,
                                         const RenderRect* rects, uint32_t rectCount) {
    std::vector<D3D11_RECT> out(rectCount);
    for (uint32_t i = 0; i < rectCount; ++i) out[i] = {rects[i].left, rects[i].top, rects[i].right, rects[i].bottom};
    callback([=, out = std::move(out)](D3D11Replay& replay) {
        if (!replay.framebuffer || !replay.framebuffer->depth) return;
        const auto* target = replay.framebuffer;
        const bool whole = out.empty() || std::any_of(out.begin(), out.end(), [&](const D3D11_RECT& r) {
            return r.left <= 0 && r.top <= 0 && r.right >= LONG(target->width) && r.bottom >= LONG(target->height);
        });
        UINT flags = (clearDepth ? D3D11_CLEAR_DEPTH : 0) | (clearStencil ? D3D11_CLEAR_STENCIL : 0);
        if (whole) {
            replay.context->ClearDepthStencilView(target->depth.Get(), flags, depthValue, UINT8(stencilValue));
            return;
        }
        // Part of the target: a full-screen triangle at that depth, scissored
        // to each rectangle, writing depth and stencil and no colour.
        auto* device = replay.device;
        if (!device->clearVertexShader) return;
        const uint32_t key = (clearDepth ? 1u : 0u) | (clearStencil ? 2u : 0u);
        auto& state = device->clearDepthStates[key];
        if (!state) {
            D3D11_DEPTH_STENCIL_DESC desc{};
            desc.DepthEnable = clearDepth;
            desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
            desc.StencilEnable = clearStencil;
            desc.StencilReadMask = desc.StencilWriteMask = 0xFF;
            desc.FrontFace = desc.BackFace = {D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE,
                                              D3D11_COMPARISON_ALWAYS};
            device->d3d->CreateDepthStencilState(&desc, &state);
        }
        auto* context = replay.context;
        const float constants[4] = {depthValue, 0, 0, 0};
        context->UpdateSubresource(device->clearConstants.Get(), 0, nullptr, constants, 0, 0);
        replay.setConstants(0, 13, device->clearConstants.Get(), 0, 0);
        // The clear's own state, then the list's back (as far as it is set).
        auto& bound = replay.bound;
        bound.forgetPipeline();
        context->VSSetShader(device->clearVertexShader.Get(), nullptr, 0);
        context->GSSetShader(nullptr, nullptr, 0);
        context->PSSetShader(nullptr, nullptr, 0);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->RSSetState(device->clearRasterizer.Get());
        const float factor[4] = {0, 0, 0, 0};
        context->OMSetBlendState(device->clearBlend.Get(), factor, 0xFFFFFFFFu);
        context->OMSetDepthStencilState(state.Get(), UINT8(stencilValue));
        const D3D11_VIEWPORT whole_viewport{0, 0, float(target->width), float(target->height), 0, 1};
        context->RSSetViewports(1, &whole_viewport);
        for (const auto& rect : out) {
            context->RSSetScissorRects(1, &rect);
            context->Draw(3, 0);
        }
        if (!replay.viewports.empty()) context->RSSetViewports(UINT(replay.viewports.size()), replay.viewports.data());
        else bound.viewportsKnown = false;
        if (!replay.scissors.empty()) context->RSSetScissorRects(UINT(replay.scissors.size()), replay.scissors.data());
        else bound.scissorsKnown = false;
        replay.applyPipeline();
    });
}

void D3D11CommandList::copyBufferRegion(RenderBufferReference dst, RenderBufferReference src, uint64_t size) {
    auto* target = const_cast<D3D11Buffer*>(static_cast<const D3D11Buffer*>(dst.ref));
    auto* source = const_cast<D3D11Buffer*>(static_cast<const D3D11Buffer*>(src.ref));
    const uint64_t dstOffset = dst.offset, srcOffset = src.offset;
    callback([=](D3D11Replay& replay) {
        if (!source->cpu.empty() && target->gpu) {
            const D3D11_BOX box{UINT(dstOffset), 0, 0, UINT(dstOffset + size), 1, 1};
            replay.context->UpdateSubresource(target->gpu.Get(), 0, &box, source->cpu.data() + srcOffset, 0, 0);
        } else if (!source->cpu.empty() && !target->cpu.empty()) {
            std::memcpy(target->cpu.data() + dstOffset, source->cpu.data() + srcOffset, size_t(size));
        } else if (source->gpu && target->gpu) {
            const D3D11_BOX box{UINT(srcOffset), 0, 0, UINT(srcOffset + size), 1, 1};
            replay.context->CopySubresourceRegion(target->gpu.Get(), 0, UINT(dstOffset), 0, 0, source->gpu.Get(), 0, &box);
        } else if (source->gpu && !target->cpu.empty()) {
            D3D11_BUFFER_DESC desc{UINT((size + 15) & ~uint64_t(15)), D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
            ComPtr<ID3D11Buffer> staging;
            if (FAILED(replay.device->d3d->CreateBuffer(&desc, nullptr, &staging))) return;
            const D3D11_BOX box{UINT(srcOffset), 0, 0, UINT(srcOffset + size), 1, 1};
            replay.context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, source->gpu.Get(), 0, &box);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(replay.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                std::memcpy(target->cpu.data() + dstOffset, mapped.pData, size_t(size));
                replay.context->Unmap(staging.Get(), 0);
            }
        }
    });
}

void D3D11CommandList::copyTextureRegion(const RenderTextureCopyLocation& dstLocation, const RenderTextureCopyLocation& srcLocation,
                                         uint32_t dstX, uint32_t dstY, uint32_t dstZ, const RenderBox* srcBox) {
    const RenderTextureCopyLocation dst = dstLocation, src = srcLocation;
    const bool hasBox = srcBox != nullptr;
    const RenderBox box = hasBox ? *srcBox : RenderBox();
    callback([=](D3D11Replay& replay) {
        auto* context = replay.context;
        if (dst.type == RenderTextureCopyType::SUBRESOURCE && src.type == RenderTextureCopyType::PLACED_FOOTPRINT) {
            // Into a texture from an upload buffer's memory.
            const auto* texture = static_cast<const D3D11Texture*>(dst.texture);
            const auto* buffer = static_cast<const D3D11Buffer*>(src.buffer);
            if (!texture || !texture->d3d || !buffer || buffer->cpu.empty()) return;
            const auto& footprint = src.placedFootprint;
            const uint32_t blockWidth = RenderFormatBlockWidth(footprint.format);
            const uint32_t rowPitch = ((footprint.rowWidth + blockWidth - 1) / blockWidth) * RenderFormatSize(footprint.format);
            const uint32_t rows = (footprint.height + blockWidth - 1) / blockWidth;
            uint32_t left = 0, top = 0, front = 0, width = footprint.width, height = footprint.height, depth = footprint.depth;
            if (hasBox) {
                left = box.left; top = box.top; front = box.front;
                width = box.right - box.left; height = box.bottom - box.top; depth = box.back - box.front;
            }
            const uint32_t mip = dst.subresource.mipLevel;
            const uint32_t mipWidth = std::max(texture->desc.width >> mip, 1u), mipHeight = std::max(texture->desc.height >> mip, 1u);
            const D3D11_BOX target{dstX, dstY, dstZ, std::min(dstX + width, mipWidth), std::min(dstY + height, mipHeight),
                                   dstZ + std::max(depth, 1u)};
            const uint8_t* data = buffer->cpu.data() + footprint.offset + size_t(top / blockWidth) * rowPitch +
                                  size_t(left / blockWidth) * RenderFormatSize(footprint.format) + size_t(front) * rowPitch * rows;
            const UINT subresource = D3D11CalcSubresource(mip, dst.subresource.arrayIndex, std::max(texture->desc.mipLevels, 1u));
            context->UpdateSubresource(texture->d3d.Get(), subresource, &target, data, rowPitch, rowPitch * rows);
        } else if (dst.type == RenderTextureCopyType::PLACED_FOOTPRINT && src.type == RenderTextureCopyType::SUBRESOURCE) {
            // Into a readback buffer: through a staging texture, at once.
            const auto* texture = static_cast<const D3D11Texture*>(src.texture);
            auto* buffer = const_cast<D3D11Buffer*>(static_cast<const D3D11Buffer*>(dst.buffer));
            if (!texture || !texture->d3d || !buffer || buffer->cpu.empty()) return;
            D3D11_TEXTURE2D_DESC desc{};
            texture->d3d->GetDesc(&desc);
            const uint32_t mip = src.subresource.mipLevel;
            desc.Width = std::max(desc.Width >> mip, 1u);
            desc.Height = std::max(desc.Height >> mip, 1u);
            desc.MipLevels = desc.ArraySize = 1;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            ComPtr<ID3D11Texture2D> staging;
            if (FAILED(replay.device->d3d->CreateTexture2D(&desc, nullptr, &staging))) return;
            const UINT subresource = D3D11CalcSubresource(mip, src.subresource.arrayIndex, std::max(texture->desc.mipLevels, 1u));
            context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture->d3d.Get(), subresource, nullptr);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return;
            const auto& footprint = dst.placedFootprint;
            const uint32_t blockWidth = RenderFormatBlockWidth(footprint.format);
            const uint32_t rowPitch = ((footprint.rowWidth + blockWidth - 1) / blockWidth) * RenderFormatSize(footprint.format);
            const uint32_t rows = (std::min(footprint.height, desc.Height) + blockWidth - 1) / blockWidth;
            const uint32_t copy = std::min(rowPitch, mapped.RowPitch);
            for (uint32_t y = 0; y < rows; ++y) {
                const size_t at = size_t(footprint.offset) + size_t(dstY / blockWidth + y) * rowPitch;
                if (at + copy > buffer->cpu.size()) break;
                std::memcpy(buffer->cpu.data() + at, static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, copy);
            }
            context->Unmap(staging.Get(), 0);
        } else if (dst.type == RenderTextureCopyType::SUBRESOURCE && src.type == RenderTextureCopyType::SUBRESOURCE) {
            const auto* target = static_cast<const D3D11Texture*>(dst.texture);
            const auto* source = static_cast<const D3D11Texture*>(src.texture);
            if (!target || !source || !target->d3d || !source->d3d) return;
            const UINT dstSub = D3D11CalcSubresource(dst.subresource.mipLevel, dst.subresource.arrayIndex,
                                                     std::max(target->desc.mipLevels, 1u));
            const UINT srcSub = D3D11CalcSubresource(src.subresource.mipLevel, src.subresource.arrayIndex,
                                                     std::max(source->desc.mipLevels, 1u));
            if (isDepth(source->desc.format)) {
                // Depth-stencil copies are of whole subresources only.
                context->CopySubresourceRegion(target->d3d.Get(), dstSub, 0, 0, 0, source->d3d.Get(), srcSub, nullptr);
                return;
            }
            D3D11_BOX region{};
            if (hasBox) region = {UINT(box.left), UINT(box.top), UINT(box.front), UINT(box.right), UINT(box.bottom), UINT(box.back)};
            context->CopySubresourceRegion(target->d3d.Get(), dstSub, dstX, dstY, dstZ, source->d3d.Get(), srcSub,
                                           hasBox ? &region : nullptr);
        }
    });
}

void D3D11CommandList::copyBuffer(const RenderBuffer* dstBuffer, const RenderBuffer* srcBuffer) {
    const auto* source = static_cast<const D3D11Buffer*>(srcBuffer);
    copyBufferRegion(RenderBufferReference(dstBuffer, 0), RenderBufferReference(srcBuffer, 0), source->desc.size);
}

void D3D11CommandList::copyTexture(const RenderTexture* dstTexture, const RenderTexture* srcTexture) {
    const auto* target = static_cast<const D3D11Texture*>(dstTexture);
    const auto* source = static_cast<const D3D11Texture*>(srcTexture);
    callback([target, source](D3D11Replay& replay) {
        if (target->d3d && source->d3d) replay.context->CopyResource(target->d3d.Get(), source->d3d.Get());
    });
}

void D3D11CommandList::resolveTexture(const RenderTexture* dstTexture, const RenderTexture* srcTexture) {
    const auto* target = static_cast<const D3D11Texture*>(dstTexture);
    const auto* source = static_cast<const D3D11Texture*>(srcTexture);
    callback([target, source](D3D11Replay& replay) {
        if (!target->d3d || !source->d3d) return;
        if (source->desc.multisampling.sampleCount > 1)
            replay.context->ResolveSubresource(target->d3d.Get(), 0, source->d3d.Get(), 0, toDXGI(target->desc.format));
        else
            replay.context->CopyResource(target->d3d.Get(), source->d3d.Get());
    });
}

void D3D11CommandList::resolveTextureRegion(const RenderTexture* dstTexture, uint32_t dstX, uint32_t dstY,
                                            const RenderTexture* srcTexture, const RenderRect* srcRect, RenderResolveMode) {
    const auto* target = static_cast<const D3D11Texture*>(dstTexture);
    const auto* source = static_cast<const D3D11Texture*>(srcTexture);
    const bool hasRect = srcRect != nullptr;
    const RenderRect rect = hasRect ? *srcRect : RenderRect();
    callback([=](D3D11Replay& replay) {
        if (!target->d3d || !source->d3d) return;
        if (source->desc.multisampling.sampleCount > 1) {
            replay.context->ResolveSubresource(target->d3d.Get(), 0, source->d3d.Get(), 0, toDXGI(target->desc.format));
            return;
        }
        const D3D11_BOX box{UINT(rect.left), UINT(rect.top), 0, UINT(rect.right), UINT(rect.bottom), 1};
        replay.context->CopySubresourceRegion(target->d3d.Get(), 0, dstX, dstY, 0, source->d3d.Get(), 0, hasRect ? &box : nullptr);
    });
}

// D3D11SwapChain

D3D11SwapChain::D3D11SwapChain(D3D11CommandQueue* queue, RenderWindow window, uint32_t textureCount, RenderFormat format,
                               uint32_t maxFrameLatency)
    : queue(queue), window(window), format(format), textureCount(std::max(textureCount, 2u)) {
    auto* device = queue->device;
    auto& factory = device->renderInterface->factory;
    ComPtr<IDXGIFactory5> factory5;
    BOOL allowTearing = FALSE;
    if (SUCCEEDED(factory.As(&factory5)))
        factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof allowTearing);
    tearing = allowTearing;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Format = toDXGI(format);
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = this->textureCount;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (const HRESULT result = factory->CreateSwapChainForHwnd(device->d3d.Get(), window, &desc, nullptr, nullptr, &d3d);
        FAILED(result)) {
        std::cerr << "NATIVE_D3D11 swap_chain=" << hresult(result) << '\n';
        d3d.Reset();
        return;
    }
    // DXGI watching the window's message queue (on another thread) and the
    // first present waited for each other on D3D12; the game handles
    // Alt+Enter and full screen itself.
    factory->MakeWindowAssociation(window, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
    ComPtr<IDXGIDevice1> dxgiDevice;
    if (SUCCEEDED(device->d3d.As(&dxgiDevice))) dxgiDevice->SetMaximumFrameLatency(std::max(maxFrameLatency, 1u));
    wrapBackBuffer();
}

void D3D11SwapChain::wrapBackBuffer() {
    textures.clear();
    ComPtr<ID3D11Texture2D> back;
    if (!d3d || FAILED(d3d->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    width = desc.Width;
    height = desc.Height;
    for (uint32_t i = 0; i < textureCount; ++i) {
        auto texture = std::make_unique<D3D11Texture>();
        texture->device = queue->device;
        texture->desc = RenderTextureDesc::ColorTarget(width, height, format);
        texture->d3d = back;
        texture->swapChain = true;
        textures.push_back(std::move(texture));
    }
}

bool D3D11SwapChain::present(uint32_t, RenderCommandSemaphore**, uint32_t) {
    std::lock_guard guard(queue->device->contextMutex);
    const UINT flags = !vsync && tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
    const HRESULT result = d3d->Present(vsync ? 1 : 0, flags);
    if (FAILED(result)) std::cerr << "NATIVE_D3D11 present=" << hresult(result) << '\n';
    return SUCCEEDED(result);
}

bool D3D11SwapChain::resize() {
    std::lock_guard guard(queue->device->contextMutex);
    RECT client{};
    GetClientRect(window, &client);
    if (client.right <= 0 || client.bottom <= 0) return false;
    textures.clear();
    queue->device->context->OMSetRenderTargets(0, nullptr, nullptr);
    queue->device->context->Flush();
    const HRESULT result = d3d->ResizeBuffers(0, UINT(client.right), UINT(client.bottom), DXGI_FORMAT_UNKNOWN,
                                              tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (FAILED(result)) std::cerr << "NATIVE_D3D11 resize=" << hresult(result) << '\n';
    wrapBackBuffer();
    return SUCCEEDED(result) && !textures.empty();
}

bool D3D11SwapChain::needsResize() const {
    RECT client{};
    if (!GetClientRect(window, &client)) return false;
    return UINT(client.right) != width || UINT(client.bottom) != height;
}

bool D3D11SwapChain::acquireTexture(RenderCommandSemaphore*, uint32_t* textureIndex) {
    if (textures.empty()) return false;
    *textureIndex = nextIndex;
    nextIndex = (nextIndex + 1) % textureCount;
    return true;
}

// D3D11CommandQueue

std::unique_ptr<RenderCommandList> D3D11CommandQueue::createCommandList() { return std::make_unique<D3D11CommandList>(this); }

std::unique_ptr<RenderSwapChain> D3D11CommandQueue::createSwapChain(RenderWindow renderWindow, uint32_t textureCount,
                                                                    RenderFormat format, uint32_t maxFrameLatency) {
    auto swapChain = std::make_unique<D3D11SwapChain>(this, renderWindow, textureCount, format, maxFrameLatency);
    if (!swapChain->d3d) return nullptr;
    return swapChain;
}

void D3D11CommandQueue::executeCommandLists(const RenderCommandList** commandLists, uint32_t commandListCount,
                                            RenderCommandSemaphore**, uint32_t, RenderCommandSemaphore**, uint32_t,
                                            RenderCommandFence* signalFence) {
    std::lock_guard guard(device->contextMutex);
    const auto started = std::chrono::steady_clock::now();
    auto* context = device->context.Get();
    // Upload memory the lists read: copied up first. Each buffer's ranges
    // are sorted and merged (a draw's constants window overlaps the next
    // draw's data in a ring), then go up once into its GPU copy; the ranges
    // read as constants are copied on from there into its constant buffer
    // (which takes part of itself only by a copy: a partial UpdateSubresource
    // of a constant buffer is an optional D3D11.1 feature).
    auto& uses = device->uploadUses;
    uses.clear();
    for (uint32_t l = 0; l < commandListCount; ++l) {
        const auto& listUses = static_cast<const D3D11CommandList*>(commandLists[l])->uses;
        uses.insert(uses.end(), listUses.begin(), listUses.end());
    }
    std::sort(uses.begin(), uses.end(), [](const D3D11BufferUse& x, const D3D11BufferUse& y) {
        return x.buffer != y.buffer ? std::less<>()(x.buffer, y.buffer) : x.begin < y.begin;
    });
    uint64_t uploaded = 0;
    // Ranges closer than this are copied as one.
    constexpr uint64_t gap = 4096;
    const auto copyUp = [&](D3D11Buffer* buffer, uint64_t begin, uint64_t end, bool constants) {
        begin &= ~uint64_t(15);
        end = std::min((end + 15) & ~uint64_t(15), buffer->desc.size);
        if (end <= begin || !buffer->gpu) return;
        const D3D11_BOX box{UINT(begin), 0, 0, UINT(end), 1, 1};
        if (!constants) {
            uploaded += end - begin;
            context->UpdateSubresource(buffer->gpu.Get(), 0, &box, buffer->cpu.data() + begin, 0, 0);
        } else if (ID3D11Buffer* target = buffer->constantBuffer()) {
            context->CopySubresourceRegion(target, 0, UINT(begin), 0, 0, buffer->gpu.Get(), 0, &box);
        }
    };
    for (size_t first = 0; first < uses.size();) {
        auto* buffer = uses[first].buffer;
        size_t last = first;
        bool constants = false;
        while (last < uses.size() && uses[last].buffer == buffer) constants |= uses[last++].constants;
        if (buffer->writeOnce) {
            if (!buffer->copiedOnce) {
                copyUp(buffer, 0, buffer->desc.size, false);
                if (constants) copyUp(buffer, 0, buffer->desc.size, true);
                buffer->copiedOnce = true;
            }
        } else if (!buffer->mapCount) {
            // Written once and unmapped: all of it, once.
            if (buffer->dirty) {
                copyUp(buffer, 0, buffer->desc.size, false);
                if (constants) copyUp(buffer, 0, buffer->desc.size, true);
                buffer->dirty = false;
            }
        } else {
            for (const bool pass : {false, true}) {
                if (pass && !constants) break;
                uint64_t begin = 0, end = 0;
                bool open = false;
                for (size_t i = first; i < last; ++i) {
                    const auto& use = uses[i];
                    if (pass && !use.constants) continue;
                    if (open && use.begin <= end + gap) {
                        end = std::max(end, use.end);
                        continue;
                    }
                    if (open) copyUp(buffer, begin, end, pass);
                    begin = use.begin;
                    end = use.end;
                    open = true;
                }
                if (open) copyUp(buffer, begin, end, pass);
            }
        }
        first = last;
    }
    const auto copied = std::chrono::steady_clock::now();
    D3D11Replay replay(device, context, device->context1.Get());
    {
        static const long trace_at = [] {
            const char* setting = std::getenv("SFR_D3D11_TRACE");
            return setting ? std::strtol(setting, nullptr, 10) : 0;
        }();
        static long submissions = 0;
        if (trace_at > 0) {
            bool draws = false;
            for (uint32_t l = 0; l < commandListCount; ++l)
                draws |= static_cast<const D3D11CommandList*>(commandLists[l])->commands.size() > 20;
            if (draws && ++submissions == trace_at) {
                replay.trace = true;
                std::cerr << "D3D11_TRACE submission=" << submissions << '\n';
            }
        }
    }
    for (uint32_t l = 0; l < commandListCount; ++l) replay.run(*static_cast<const D3D11CommandList*>(commandLists[l]));
    if (signalFence) {
        auto* fence = static_cast<D3D11CommandFence*>(signalFence);
        context->End(fence->query.Get());
        fence->pending = true;
    }
    context->Flush();
    if (replay.draws) {
        const auto now = std::chrono::steady_clock::now();
        device->noteReplay({std::chrono::duration<float, std::milli>(now - started).count(),
                            std::chrono::duration<float, std::milli>(copied - started).count(), replay.draws, replay.calls,
                            replay.skipped, uploaded + replay.windowBytes});
    }
}

void D3D11Device::noteReplay(const D3D11ReplayStats& stats) {
    // NATIVE_D3D11_REPLAY: every 600 submissions that draw, how long putting
    // them on the immediate context took (ms: copies up, replay and flush;
    // upload_ms: the copies up alone), the draws, the binding calls made and
    // those skipped as bound already, and the upload bytes copied up.
    replayStats.push_back(stats);
    if (replayStats.size() < 600) return;
    const size_t count = replayStats.size();
    std::vector<float> sorted;
    double total = 0, upload = 0, draws = 0, calls = 0, skipped = 0, bytes = 0;
    for (const auto& item : replayStats) {
        sorted.push_back(item.milliseconds);
        total += item.milliseconds;
        upload += item.uploadMilliseconds;
        draws += item.draws;
        calls += item.calls;
        skipped += item.skipped;
        bytes += double(item.uploadBytes);
    }
    std::sort(sorted.begin(), sorted.end());
    std::cerr << "NATIVE_D3D11_REPLAY submissions=" << count << " draws_mean=" << draws / count << " ms_mean=" << total / count
              << " ms_median=" << sorted[count / 2] << " ms_p95=" << sorted[count * 95 / 100] << " ms_max=" << sorted.back()
              << " upload_ms_mean=" << upload / count << " upload_kb_mean=" << bytes / count / 1024
              << " calls_mean=" << calls / count << " skipped_mean=" << skipped / count << '\n';
    replayStats.clear();
}

void D3D11CommandQueue::waitForCommandFence(RenderCommandFence* signalFence) {
    auto* fence = static_cast<D3D11CommandFence*>(signalFence);
    if (!fence || !fence->pending) return;
    for (;;) {
        BOOL done = FALSE;
        HRESULT result;
        {
            std::lock_guard guard(device->contextMutex);
            result = device->context->GetData(fence->query.Get(), &done, sizeof done, 0);
        }
        if (result == S_OK || FAILED(result)) break;
        Sleep(0);
    }
    fence->pending = false;
}
}
