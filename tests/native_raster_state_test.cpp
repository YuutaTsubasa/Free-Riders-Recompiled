#include "native_graphics.h"
#include "native_raster_state.h"

#include "plume_d3d12.h"
#include "plume_render_interface.h"

#define NOMINMAX
#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void require_hr(HRESULT result, const char* message) {
    if (FAILED(result)) throw std::runtime_error(std::string(message) + " (HRESULT " + std::to_string(result) + ")");
}

template<class Exception, class Operation>
bool rejects_with(Operation operation) {
    try { operation(); } catch (const Exception&) { return true; }
    return false;
}

void construction_and_validation_retain_only_valid_state() {
    sfr::NativeGraphics graphics;
    graphics.initialize();
    require(rejects_with<std::invalid_argument>([&] { sfr::NativeRasterState state(graphics, 0, 1); }), "zero width is rejected");
    require(rejects_with<std::invalid_argument>([&] { sfr::NativeRasterState state(graphics, 1, 8193); }), "excessive height is rejected");

    sfr::NativeRasterState state(graphics, 8, 6);
    D3D12_FEATURE_DATA_D3D12_OPTIONS13 actual_options{};
    auto* actual_device = static_cast<plume::D3D12Device*>(&graphics.device())->d3d;
    require_hr(actual_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS13, &actual_options, sizeof(actual_options)),
               "query actual D3D12_OPTIONS13 capability in test");
    require(state.inverted_depth_supported() == (actual_options.InvertedViewportDepthFlipsZSupported != FALSE),
            "reported inverted-depth support exactly matches the native device query");
    require(state.viewport() == plume::RenderViewport(0, 0, 8, 6, 0, 1), "constructor retains a full-target viewport");
    require(state.scissor() == plume::RenderRect(0, 0, 8, 6), "constructor retains a full-target scissor");

    const plume::RenderViewport valid(1, 2, 4, 3, 0.25f, 0.75f);
    const plume::RenderRect valid_scissor(2, 2, 5, 5);
    state.set(valid, valid_scissor);
    require(state.viewport() == valid && state.scissor() == valid_scissor, "valid state is retained exactly");

    const std::array<plume::RenderViewport, 6> invalid_viewports{{
        {std::numeric_limits<float>::quiet_NaN(), 0, 1, 1}, {0, 0, -1, 1},
        {32768, 0, 1, 1}, {32767, 0, 2, 1}, {0, 0, 1, 1, -0.01f, 1}, {0, 0, 1, 1, 0, 1.01f}}};
    for (const auto& viewport : invalid_viewports) {
        require(rejects_with<std::invalid_argument>([&] { state.set(viewport, valid_scissor); }), "invalid viewport is rejected");
        require(state.viewport() == valid && state.scissor() == valid_scissor, "invalid viewport preserves prior state");
    }
    const std::array<plume::RenderRect, 4> invalid_scissors{{{-1, 0, 1, 1}, {0, 0, 9, 1}, {2, 1, 1, 2}, {0, 4, 1, 3}}};
    for (const auto& scissor : invalid_scissors) {
        require(rejects_with<std::invalid_argument>([&] { state.set(plume::RenderViewport(0, 0, 1, 1), scissor); }), "invalid scissor is rejected");
        require(state.viewport() == valid && state.scissor() == valid_scissor, "invalid scissor preserves prior state");
    }
    state.set(plume::RenderViewport(0, 0, 1, 1), plume::RenderRect(3, 3, 3, 3));
    require(state.scissor() == plume::RenderRect(3, 3, 3, 3), "ordered empty scissor is accepted");

    const plume::RenderViewport reverse(0, 0, 8, 6, 1, 0);
    if (state.inverted_depth_supported()) {
        state.set(reverse, plume::RenderRect(0, 0, 8, 6));
        require(state.viewport() == reverse, "reported inverted-depth support accepts reverse depth");
    } else {
        const auto before_viewport = state.viewport();
        const auto before_scissor = state.scissor();
        require(rejects_with<std::runtime_error>([&] { state.set(reverse, plume::RenderRect(0, 0, 8, 6)); }),
                "actual lack of inverted-depth support explicitly rejects reverse depth");
        require(state.viewport() == before_viewport && state.scissor() == before_scissor,
                "unsupported reverse depth preserves prior state");
    }
}

ComPtr<ID3DBlob> compile_shader(const char* source, const char* entry, const char* target) {
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, entry, target,
                                      D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
    if (FAILED(result)) {
        const std::string detail = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "unknown error";
        throw std::runtime_error("test shader compilation failed: " + detail);
    }
    return code;
}

struct DrawReadback {
    std::vector<std::array<uint8_t, 4>> color;
    std::vector<float> depth;
};

DrawReadback draw_triangle(sfr::NativeGraphics& graphics, const sfr::NativeRasterState& state, uint32_t width, uint32_t height) {
    auto* device = static_cast<plume::D3D12Device*>(&graphics.device())->d3d;
    require(device != nullptr, "native device is available");
    auto commands = graphics.queue().createCommandList();
    auto fence = graphics.device().createCommandFence();
    require(commands && fence, "draw command objects are created");
    auto* list = static_cast<plume::D3D12CommandList*>(commands.get())->d3d;
    require(list != nullptr, "native draw command list is available");

    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12DescriptorHeap> dsv_heap;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    require_hr(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap)), "create RTV heap");
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    require_hr(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&dsv_heap)), "create DSV heap");

    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC color_desc{};
    color_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    color_desc.Width = width;
    color_desc.Height = height;
    color_desc.DepthOrArraySize = 1;
    color_desc.MipLevels = 1;
    color_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    color_desc.SampleDesc.Count = 1;
    color_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    color_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE color_clear{DXGI_FORMAT_R8G8B8A8_UNORM, {0, 0, 0, 1}};
    ComPtr<ID3D12Resource> color;
    require_hr(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &color_desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &color_clear, IID_PPV_ARGS(&color)), "create color target");
    device->CreateRenderTargetView(color.Get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());

    auto depth_desc = color_desc;
    depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
    depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE depth_clear{};
    depth_clear.Format = DXGI_FORMAT_D32_FLOAT;
    depth_clear.DepthStencil.Depth = 0.0f;
    ComPtr<ID3D12Resource> depth;
    require_hr(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &depth_desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear, IID_PPV_ARGS(&depth)), "create depth target");
    device->CreateDepthStencilView(depth.Get(), nullptr, dsv_heap->GetCPUDescriptorHandleForHeapStart());

    constexpr char shader_source[] =
        "struct V { float4 p : SV_Position; };"
        "V vs(uint id : SV_VertexID) { float2 p[3] = {float2(-1,-1),float2(-1,3),float2(3,-1)}; V o; o.p=float4(p[id],0.25,1); return o; }"
        "float4 ps() : SV_Target { return float4(1,0,0,1); }";
    auto vs = compile_shader(shader_source, "vs", "vs_5_0");
    auto ps = compile_shader(shader_source, "ps", "ps_5_0");
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> root_blob;
    ComPtr<ID3DBlob> root_errors;
    require_hr(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob, &root_errors), "serialize root signature");
    ComPtr<ID3D12RootSignature> root;
    require_hr(device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(), IID_PPV_ARGS(&root)), "create root signature");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root.Get();
    pso_desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso_desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.DepthStencilState.DepthEnable = TRUE;
    pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pso_desc.DepthStencilState.StencilEnable = FALSE;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso_desc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    require_hr(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso)), "create graphics pipeline");

    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 2> footprints{};
    UINT rows = 0;
    UINT64 row_bytes = 0;
    UINT64 color_size = 0;
    device->GetCopyableFootprints(&color_desc, 0, 1, 0, &footprints[0], &rows, &row_bytes, &color_size);
    UINT64 depth_size = 0;
    device->GetCopyableFootprints(&depth_desc, 0, 1, 0, &footprints[1], &rows, &row_bytes, &depth_size);
    constexpr UINT64 placement_alignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    footprints[1].Offset = (color_size + placement_alignment - 1) & ~(placement_alignment - 1);
    const UINT64 total_size = footprints[1].Offset + depth_size;
    require((footprints[1].Offset % placement_alignment) == 0, "depth footprint has required placement alignment");
    require(rows > 0 && total_size >= footprints[1].Offset +
                static_cast<UINT64>(footprints[1].Footprint.RowPitch) * (rows - 1) + row_bytes,
            "readback allocation contains the complete depth footprint");
    D3D12_HEAP_PROPERTIES readback_heap{};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer_desc{};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = total_size;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    require_hr(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "create readback buffer");

    commands->begin();
    const auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    const auto dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
    list->ClearRenderTargetView(rtv, color_clear.Color, 0, nullptr);
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0, 0, 0, nullptr);
    list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    list->SetGraphicsRootSignature(root.Get());
    list->SetPipelineState(pso.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    state.apply(*commands);
    list->DrawInstanced(3, 1, 0, 0);
    std::array<D3D12_RESOURCE_BARRIER, 2> barriers{};
    barriers[0].Type = barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition = {color.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
    barriers[1].Transition = {depth.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    destination.PlacedFootprint = footprints[0]; source.pResource = color.Get();
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    destination.PlacedFootprint = footprints[1]; source.pResource = depth.Get();
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    commands->end();
    graphics.queue().executeCommandLists(commands.get(), fence.get());
    graphics.queue().waitForCommandFence(fence.get());

    const uint8_t* bytes = nullptr;
    const HRESULT map_result = readback->Map(0, nullptr, reinterpret_cast<void**>(const_cast<uint8_t**>(&bytes)));
    if (FAILED(map_result))
        throw std::runtime_error("map draw readback (HRESULT " + std::to_string(map_result) +
                                 ", device reason " + std::to_string(device->GetDeviceRemovedReason()) + ")");
    DrawReadback result;
    result.color.resize(static_cast<size_t>(width) * height);
    result.depth.resize(static_cast<size_t>(width) * height);
    for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
        std::memcpy(result.color[static_cast<size_t>(y) * width + x].data(),
                    bytes + footprints[0].Offset + static_cast<size_t>(y) * footprints[0].Footprint.RowPitch + x * 4, 4);
        std::memcpy(&result.depth[static_cast<size_t>(y) * width + x],
                    bytes + footprints[1].Offset + static_cast<size_t>(y) * footprints[1].Footprint.RowPitch + x * 4, 4);
    }
    readback->Unmap(0, nullptr);
    return result;
}

void actual_gpu_draw_obeys_retained_viewport_scissor_and_depth_mapping() {
    constexpr uint32_t width = 8, height = 8;
    sfr::NativeGraphics graphics;
    graphics.initialize();
    sfr::NativeRasterState state(graphics, width, height);
    const plume::RenderRect scissor(2, 0, 7, 6);
    state.set(plume::RenderViewport(1, 1, 4, 4, 0, 1), scissor);
    const auto normal = draw_triangle(graphics, state, width, height);
    for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
        const bool covered = x >= 2 && x < 5 && y >= 1 && y < 5;
        const auto index = static_cast<size_t>(y) * width + x;
        require(normal.color[index] == (covered ? std::array<uint8_t, 4>{255, 0, 0, 255} : std::array<uint8_t, 4>{0, 0, 0, 255}),
                "actual GPU color coverage is viewport intersect scissor");
        require(normal.depth[index] == (covered ? 0.25f : 0.0f), "normal viewport maps synthetic z 0.25 to depth 0.25");
    }

    if (state.inverted_depth_supported()) {
        state.set(plume::RenderViewport(1, 1, 4, 4, 1, 0), scissor);
        const auto reverse = draw_triangle(graphics, state, width, height);
        for (uint32_t y = 1; y < 5; ++y) for (uint32_t x = 2; x < 5; ++x)
            require(reverse.depth[static_cast<size_t>(y) * width + x] == 0.75f,
                    "inverted viewport maps synthetic z 0.25 to depth 0.75 on a new command list");
    }
}
}

int main() {
    // Every check here drives D3D12 directly.
    if (sfr::selected_graphics_backend() != sfr::GraphicsBackend::d3d12) {
        std::cout << "SKIP native raster state checks (D3D12 only)\n";
        return 0;
    }
    try {
        construction_and_validation_retain_only_valid_state();
        actual_gpu_draw_obeys_retained_viewport_scissor_and_depth_mapping();
        std::cout << "Native raster state checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
