// Plume's render interface on Direct3D 11 (plume_d3d11.h). Issue #359.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "plume_d3d11.h"

#include <d3dcompiler.h>

#include <algorithm>
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
}

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
    // SFR_D3D11_TRACE=<n>: the n-th submission with draws logs each draw's
    // state and how many samples it wrote (an occlusion query a draw).
    bool trace = false;
    uint32_t traced = 0;
    template <typename Draw>
    void draw(const char* kind, uint32_t count, Draw&& issue) {
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
        UINT viewports = 1;
        context->RSGetViewports(&viewports, &viewport);
        D3D11_RECT scissor{};
        UINT scissors = 1;
        context->RSGetScissorRects(&scissors, &scissor);
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
    void applyPipeline() {
        if (!pipeline) return;
        context->VSSetShader(pipeline->vs.Get(), nullptr, 0);
        context->GSSetShader(pipeline->gs.Get(), nullptr, 0);
        context->PSSetShader(pipeline->ps.Get(), nullptr, 0);
        context->IASetInputLayout(pipeline->inputLayout.Get());
        context->IASetPrimitiveTopology(pipeline->topology);
        context->RSSetState(pipeline->rasterizer.Get());
        const float factor[4] = {1, 1, 1, 1};
        context->OMSetBlendState(pipeline->blend.Get(), factor, 0xFFFFFFFFu);
        context->OMSetDepthStencilState(pipeline->depthStencil.Get(),
                                        stencilOverride ? stencilReference : pipeline->stencilReference);
    }
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
    if (!constantBufferOffsets) {
        // The renderer's constants are ranges of its upload ring.
        std::cerr << "NATIVE_D3D11 refused=no-constant-buffer-offsets (D3D11.1 runtime and driver needed)\n";
        d3d.Reset();
        return;
    }

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
    return shader;
}

std::vector<uint8_t> D3D11Device::shaderCode(const D3D11Shader* shader, const char* profile, uint32_t specConstants) {
    if (!shader->source) return shader->code;
    const uint64_t hash = fnv1a(shader->code.data(), shader->code.size());
    char key[128];
    std::snprintf(key, sizeof key, "%016llx-%zu-%s-%s-%u", static_cast<unsigned long long>(hash), shader->code.size(),
                  shader->entryPoint.c_str(), profile, specConstants);
    {
        std::lock_guard guard(shaderMutex);
        if (const auto found = compiledShaders.find(key); found != compiledShaders.end()) return found->second;
    }
    const auto folder = shaderCacheFolder();
    const auto file = folder / (std::string(key) + ".dxbc");
    std::vector<uint8_t> code;
    if (std::ifstream in{file, std::ios::binary}) code.assign(std::istreambuf_iterator<char>(in), {});
    if (code.size() < 4 || std::memcmp(code.data(), "DXBC", 4) != 0) {
        const std::string text(shader->code.begin(), shader->code.end());
        const std::string spec = std::to_string(specConstants);
        const std::vector<D3D_SHADER_MACRO> macros{{"SFR_SPEC_CONSTANTS", spec.c_str()}, {nullptr, nullptr}};
        std::string errors;
        const auto started = std::chrono::steady_clock::now();
        code = compile(text, shader->entryPoint.c_str(), profile, macros, &errors);
        const double milliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (code.empty()) {
            for (char& c : errors) if (c == '\n' || c == '\r') c = ' ';
            std::cerr << "NATIVE_D3D11_SHADER compiled=0 key=" << key << " errors=" << errors.substr(0, 600) << '\n';
            return {};
        }
        std::cerr << "NATIVE_D3D11_SHADER compiled=1 key=" << key << " ms=" << int(milliseconds) << '\n';
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(code.data()), std::streamsize(code.size()));
    }
    std::lock_guard guard(shaderMutex);
    compiledShaders.emplace(key, code);
    return code;
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
    uses.clear();
    recordingLayout = nullptr;
}

void D3D11CommandList::drawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance) {
    commands.push_back([=](D3D11Replay& replay) {
        if (!replay.pipeline || !replay.pipeline->valid) return;
        replay.draw("vertices", vertexCount, [&] {
            if (instanceCount == 1 && startInstance == 0) replay.context->Draw(vertexCount, startVertex);
            else replay.context->DrawInstanced(vertexCount, instanceCount, startVertex, startInstance);
        });
    });
}

void D3D11CommandList::drawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t startIndex,
                                            int32_t baseVertex, uint32_t startInstance) {
    commands.push_back([=](D3D11Replay& replay) {
        if (!replay.pipeline || !replay.pipeline->valid) return;
        replay.draw("indices", indexCount, [&] {
            if (instanceCount == 1 && startInstance == 0) replay.context->DrawIndexed(indexCount, startIndex, baseVertex);
            else replay.context->DrawIndexedInstanced(indexCount, instanceCount, startIndex, baseVertex, startInstance);
        });
    });
}

void D3D11CommandList::setPipeline(const RenderPipeline* pipeline) {
    const auto* graphics = static_cast<const D3D11GraphicsPipeline*>(pipeline);
    commands.push_back([graphics](D3D11Replay& replay) {
        replay.pipeline = graphics;
        replay.stencilOverride = false;
        replay.applyPipeline();
    });
}

void D3D11CommandList::setStencilReference(uint32_t reference) {
    commands.push_back([reference](D3D11Replay& replay) {
        replay.stencilReference = reference;
        replay.stencilOverride = true;
        if (replay.pipeline)
            replay.context->OMSetDepthStencilState(replay.pipeline->depthStencil.Get(), reference);
    });
}

void D3D11CommandList::setGraphicsPipelineLayout(const RenderPipelineLayout* pipelineLayout) {
    recordingLayout = static_cast<const D3D11PipelineLayout*>(pipelineLayout);
}

void D3D11CommandList::setGraphicsPushConstants(uint32_t rangeIndex, const void* data, uint32_t offset, uint32_t size) {
    const auto* layout = recordingLayout;
    if (!layout || rangeIndex >= layout->pushConstantRanges.size()) return;
    const auto& range = layout->pushConstantRanges[rangeIndex];
    if (!size) size = range.size;
    std::vector<uint8_t> bytes(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    ID3D11Buffer* buffer = layout->pushConstantBuffers[rangeIndex].Get();
    const uint32_t reg = range.binding, total = range.size;
    commands.push_back([buffer, reg, offset, total, bytes = std::move(bytes)](D3D11Replay& replay) {
        // Constant buffers update whole (D3D11.1's partial updates aside).
        std::vector<uint8_t> whole((total + 15u) & ~15u, 0);
        std::memcpy(whole.data() + offset, bytes.data(), std::min<size_t>(bytes.size(), whole.size() - offset));
        replay.context->UpdateSubresource(buffer, 0, nullptr, whole.data(), 0, 0);
        replay.context->VSSetConstantBuffers(reg, 1, &buffer);
        replay.context->GSSetConstantBuffers(reg, 1, &buffer);
        replay.context->PSSetConstantBuffers(reg, 1, &buffer);
    });
}

void D3D11CommandList::setGraphicsDescriptorSet(RenderDescriptorSet* descriptorSet, uint32_t) {
    const auto* set = static_cast<const D3D11DescriptorSet*>(descriptorSet);
    commands.push_back([set](D3D11Replay& replay) {
        for (const auto& entry : set->entries) {
            switch (entry.type) {
            case RenderDescriptorRangeType::TEXTURE: {
                if (entry.reg >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) break;
                ID3D11ShaderResourceView* view = entry.srv.Get();
                replay.context->VSSetShaderResources(entry.reg, 1, &view);
                replay.context->PSSetShaderResources(entry.reg, 1, &view);
                break;
            }
            case RenderDescriptorRangeType::SAMPLER: {
                if (entry.reg >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) break;
                ID3D11SamplerState* sampler = entry.sampler.Get();
                replay.context->VSSetSamplers(entry.reg, 1, &sampler);
                replay.context->PSSetSamplers(entry.reg, 1, &sampler);
                break;
            }
            default: break;
            }
        }
    });
}

void D3D11CommandList::setXenosTextures(const RenderDescriptorSet* textures, const RenderDescriptorSet* samplers,
                                        const std::array<uint32_t, 64>& indices) {
    const auto* textureSet = static_cast<const D3D11DescriptorSet*>(textures);
    const auto* samplerSet = static_cast<const D3D11DescriptorSet*>(samplers);
    commands.push_back([textureSet, samplerSet, indices](D3D11Replay& replay) {
        std::array<ID3D11ShaderResourceView*, 48> views{};
        std::array<ID3D11SamplerState*, 16> states{};
        for (uint32_t i = 0; i < 48; ++i)
            if (indices[i] < textureSet->entries.size()) views[i] = textureSet->entries[indices[i]].srv.Get();
        for (uint32_t i = 0; i < 16; ++i)
            if (indices[48 + i] < samplerSet->entries.size()) states[i] = samplerSet->entries[indices[48 + i]].sampler.Get();
        replay.context->VSSetShaderResources(0, 48, views.data());
        replay.context->PSSetShaderResources(0, 48, views.data());
        replay.context->VSSetSamplers(0, 16, states.data());
        replay.context->PSSetSamplers(0, 16, states.data());
    });
}

void D3D11CommandList::setGraphicsRootDescriptor(RenderBufferReference reference, uint32_t rootDescriptorIndex) {
    const auto* layout = recordingLayout;
    if (!layout || rootDescriptorIndex >= layout->rootDescriptors.size() || !reference.ref) return;
    auto* buffer = const_cast<D3D11Buffer*>(static_cast<const D3D11Buffer*>(reference.ref));
    const uint32_t reg = layout->rootDescriptors[rootDescriptorIndex].shaderRegister;
    // A window of up to 4096 constants at the offset, in whole 16-constant
    // (256-byte) steps, as D3D11.1 binds them.
    const uint64_t offset = reference.offset & ~uint64_t(255);
    const uint64_t available = buffer->desc.size > offset ? buffer->desc.size - offset : 0;
    const UINT first = UINT(offset / 16);
    const UINT count = UINT(std::min<uint64_t>(4096, (available / 16) & ~uint64_t(15)));
    if (!count) return;
    if (!buffer->cpu.empty()) uses.push_back({buffer, offset, offset + uint64_t(count) * 16, true});
    commands.push_back([buffer, reg, first, count](D3D11Replay& replay) {
        ID3D11Buffer* constants = buffer->constantBuffer();
        replay.context1->VSSetConstantBuffers1(reg, 1, &constants, &first, &count);
        replay.context1->PSSetConstantBuffers1(reg, 1, &constants, &first, &count);
    });
}

void D3D11CommandList::setIndexBuffer(const RenderIndexBufferView* view) {
    if (!view || !view->buffer.ref) {
        commands.push_back([](D3D11Replay& replay) { replay.context->IASetIndexBuffer(nullptr, DXGI_FORMAT_R32_UINT, 0); });
        return;
    }
    auto* buffer = const_cast<D3D11Buffer*>(static_cast<const D3D11Buffer*>(view->buffer.ref));
    const UINT offset = UINT(view->buffer.offset);
    const DXGI_FORMAT format = toDXGI(view->format);
    if (!buffer->cpu.empty()) uses.push_back({buffer, offset, offset + uint64_t(view->size), false});
    commands.push_back([buffer, offset, format](D3D11Replay& replay) {
        replay.context->IASetIndexBuffer(buffer->gpu.Get(), format, offset);
    });
}

void D3D11CommandList::setVertexBuffers(uint32_t startSlot, const RenderVertexBufferView* views, uint32_t viewCount,
                                        const RenderInputSlot* inputSlots) {
    std::vector<ID3D11Buffer*> buffers(viewCount);
    std::vector<UINT> strides(viewCount), offsets(viewCount);
    std::vector<D3D11Buffer*> owners(viewCount);
    for (uint32_t i = 0; i < viewCount; ++i) {
        auto* buffer = const_cast<D3D11Buffer*>(static_cast<const D3D11Buffer*>(views[i].buffer.ref));
        owners[i] = buffer;
        offsets[i] = UINT(views[i].buffer.offset);
        for (uint32_t j = 0; inputSlots && j < viewCount; ++j)
            if (inputSlots[j].index == startSlot + i) strides[i] = inputSlots[j].stride;
        if (buffer && !buffer->cpu.empty() && views[i].size)
            uses.push_back({buffer, views[i].buffer.offset, views[i].buffer.offset + views[i].size, false});
    }
    commands.push_back([startSlot, owners, strides, offsets](D3D11Replay& replay) {
        std::vector<ID3D11Buffer*> buffers(owners.size());
        for (size_t i = 0; i < owners.size(); ++i) buffers[i] = owners[i] ? owners[i]->gpu.Get() : nullptr;
        replay.context->IASetVertexBuffers(startSlot, UINT(buffers.size()), buffers.data(), strides.data(), offsets.data());
    });
}

void D3D11CommandList::setViewports(const RenderViewport* viewports, uint32_t count) {
    std::vector<D3D11_VIEWPORT> out(count);
    for (uint32_t i = 0; i < count; ++i)
        out[i] = {viewports[i].x, viewports[i].y, viewports[i].width, viewports[i].height, viewports[i].minDepth,
                  viewports[i].maxDepth};
    commands.push_back([out = std::move(out)](D3D11Replay& replay) {
        replay.viewports = out;
        replay.context->RSSetViewports(UINT(out.size()), out.data());
    });
}

void D3D11CommandList::setScissors(const RenderRect* rects, uint32_t count) {
    std::vector<D3D11_RECT> out(count);
    for (uint32_t i = 0; i < count; ++i) out[i] = {rects[i].left, rects[i].top, rects[i].right, rects[i].bottom};
    commands.push_back([out = std::move(out)](D3D11Replay& replay) {
        replay.scissors = out;
        replay.context->RSSetScissorRects(UINT(out.size()), out.data());
    });
}

void D3D11CommandList::setFramebuffer(const RenderFramebuffer* framebuffer) {
    const auto* target = static_cast<const D3D11Framebuffer*>(framebuffer);
    commands.push_back([target](D3D11Replay& replay) {
        replay.framebuffer = target;
        if (!target) {
            replay.context->OMSetRenderTargets(0, nullptr, nullptr);
            return;
        }
        // The runtime unbinds views of these textures that shaders read.
        std::array<ID3D11RenderTargetView*, 8> colors{};
        for (size_t i = 0; i < target->colors.size() && i < colors.size(); ++i) colors[i] = target->colors[i].Get();
        replay.context->OMSetRenderTargets(UINT(target->colors.size()), colors.data(), target->depth.Get());
    });
}

void D3D11CommandList::clearColor(uint32_t attachmentIndex, RenderColor color, const RenderRect* rects, uint32_t rectCount) {
    std::vector<D3D11_RECT> out(rectCount);
    for (uint32_t i = 0; i < rectCount; ++i) out[i] = {rects[i].left, rects[i].top, rects[i].right, rects[i].bottom};
    commands.push_back([attachmentIndex, color, out = std::move(out)](D3D11Replay& replay) {
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
    commands.push_back([=, out = std::move(out)](D3D11Replay& replay) {
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
        const float constants[4] = {depthValue, 0, 0, 0};
        replay.context->UpdateSubresource(device->clearConstants.Get(), 0, nullptr, constants, 0, 0);
        ID3D11Buffer* buffer = device->clearConstants.Get();
        replay.context->VSSetConstantBuffers(13, 1, &buffer);
        replay.context->VSSetShader(device->clearVertexShader.Get(), nullptr, 0);
        replay.context->GSSetShader(nullptr, nullptr, 0);
        replay.context->PSSetShader(nullptr, nullptr, 0);
        replay.context->IASetInputLayout(nullptr);
        replay.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        replay.context->RSSetState(device->clearRasterizer.Get());
        const float factor[4] = {1, 1, 1, 1};
        replay.context->OMSetBlendState(device->clearBlend.Get(), factor, 0xFFFFFFFFu);
        replay.context->OMSetDepthStencilState(state.Get(), UINT8(stencilValue));
        const D3D11_VIEWPORT whole_viewport{0, 0, float(target->width), float(target->height), 0, 1};
        replay.context->RSSetViewports(1, &whole_viewport);
        for (const auto& rect : out) {
            replay.context->RSSetScissorRects(1, &rect);
            replay.context->Draw(3, 0);
        }
        if (!replay.viewports.empty()) replay.context->RSSetViewports(UINT(replay.viewports.size()), replay.viewports.data());
        if (!replay.scissors.empty()) replay.context->RSSetScissorRects(UINT(replay.scissors.size()), replay.scissors.data());
        replay.applyPipeline();
    });
}

void D3D11CommandList::copyBufferRegion(RenderBufferReference dst, RenderBufferReference src, uint64_t size) {
    auto* target = const_cast<D3D11Buffer*>(static_cast<const D3D11Buffer*>(dst.ref));
    auto* source = const_cast<D3D11Buffer*>(static_cast<const D3D11Buffer*>(src.ref));
    const uint64_t dstOffset = dst.offset, srcOffset = src.offset;
    commands.push_back([=](D3D11Replay& replay) {
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
    commands.push_back([=](D3D11Replay& replay) {
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
    commands.push_back([target, source](D3D11Replay& replay) {
        if (target->d3d && source->d3d) replay.context->CopyResource(target->d3d.Get(), source->d3d.Get());
    });
}

void D3D11CommandList::resolveTexture(const RenderTexture* dstTexture, const RenderTexture* srcTexture) {
    const auto* target = static_cast<const D3D11Texture*>(dstTexture);
    const auto* source = static_cast<const D3D11Texture*>(srcTexture);
    commands.push_back([target, source](D3D11Replay& replay) {
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
    commands.push_back([=](D3D11Replay& replay) {
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
    auto* context = device->context.Get();
    // Upload memory the lists read: copied up first, merged per buffer.
    std::map<std::pair<D3D11Buffer*, bool>, std::pair<uint64_t, uint64_t>> ranges;
    for (uint32_t l = 0; l < commandListCount; ++l)
        for (const auto& use : static_cast<const D3D11CommandList*>(commandLists[l])->uses) {
            const auto key = std::make_pair(use.buffer, use.constants);
            auto [it, made] = ranges.try_emplace(key, use.begin, use.end);
            if (!made) {
                it->second.first = std::min(it->second.first, use.begin);
                it->second.second = std::max(it->second.second, use.end);
            }
        }
    for (auto& [key, range] : ranges) {
        auto* buffer = key.first;
        if (!buffer->mapCount) {
            // Written once and unmapped: all of it, once.
            if (!buffer->dirty) continue;
            range = {0, buffer->desc.size};
        }
        ID3D11Buffer* target = key.second ? buffer->constantBuffer() : buffer->gpu.Get();
        if (!target) continue;
        uint64_t begin = range.first & ~uint64_t(15), end = std::min((range.second + 15) & ~uint64_t(15), buffer->desc.size);
        if (end <= begin) continue;
        if (key.second) {
            // A constant buffer takes part of itself only by a copy (partial
            // UpdateSubresource of one is an optional D3D11.1 feature): the
            // range goes up into the other copy first.
            const D3D11_BOX box{UINT(begin), 0, 0, UINT(end), 1, 1};
            if (!buffer->gpu) continue;
            context->UpdateSubresource(buffer->gpu.Get(), 0, &box, buffer->cpu.data() + begin, 0, 0);
            context->CopySubresourceRegion(target, 0, UINT(begin), 0, 0, buffer->gpu.Get(), 0, &box);
        } else {
            D3D11_BOX box{UINT(begin), 0, 0, UINT(end), 1, 1};
            context->UpdateSubresource(target, 0, &box, buffer->cpu.data() + begin, 0, 0);
        }
    }
    for (auto& [key, range] : ranges)
        if (!key.first->mapCount) key.first->dirty = false;
    D3D11Replay replay{device, context, device->context1.Get()};
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
    for (uint32_t l = 0; l < commandListCount; ++l)
        for (const auto& command : static_cast<const D3D11CommandList*>(commandLists[l])->commands) command(replay);
    if (signalFence) {
        auto* fence = static_cast<D3D11CommandFence*>(signalFence);
        context->End(fence->query.Get());
        fence->pending = true;
    }
    context->Flush();
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
