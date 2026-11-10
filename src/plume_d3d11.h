#pragma once
// A Direct3D 11 backend for Plume's render interface (Issue #359): the game on
// GPUs whose Windows drivers have neither Vulkan nor D3D12 (Intel HD 4000 and
// the like), at feature level 11_0. Experimental; Windows only.
//
// It lives in this repository, not in tools/Plume (the pinned upstream), and
// implements only what the game's renderer and presentation use:
//  - Command lists record (draws and bindings as plain data, the rest as
//    callbacks); executeCommandLists replays them on the immediate context,
//    so everything the GPU sees is ordered as recorded. The replay keeps
//    what the context has bound and calls D3D11 only for what changes.
//  - Upload buffers are CPU memory (map() hands it out, persistently) with
//    GPU copies: before a list replays, the ranges its draws and copies read
//    are copied up (UpdateSubresource), which also makes the CPU memory
//    reusable at once. A buffer used as constants has a copy of its own
//    (D3D11 forbids constant buffers bound as anything else), bound at an
//    offset with D3D11.1's *SetConstantBuffers1. A driver without constant
//    buffer offsets (or SFR_D3D11_NO_CB_OFFSET=1) gets each window copied
//    instead, at the draw, into a dynamic constant buffer of its register.
//  - Copies into a texture from an upload buffer are UpdateSubresource from
//    the CPU memory; into a readback buffer, a staging texture mapped at once.
//  - Shaders are DXBC, or HLSL text (not starting with "DXBC") that each
//    pipeline compiles with d3dcompiler_47 for its specialization constant
//    (SFR_SPEC_CONSTANTS), with a disk cache. Shaders compiled ahead of time
//    (shaders-d3d11.pack) come from D3D11PrecompiledShader first.
//  - Descriptor sets are arrays of views. Generic sets bind their textures and
//    samplers at their binding numbers, push constants at a constant buffer
//    of their binding number, root descriptors at their register. The game's
//    Xenos draws bind 48 textures and 16 samplers of their own instead
//    (D3D11CommandList::setXenosTextures).
//  - Fences are event queries; semaphores do nothing (one queue).
#ifdef _WIN32

#include "plume_render_interface.h"

#include <d3d11_1.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <array>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace plume {
using Microsoft::WRL::ComPtr;

struct D3D11Device;
struct D3D11CommandQueue;
struct D3D11Replay;

struct D3D11Interface : RenderInterface {
    // SFR_D3D11_WARP=1: the software rasterizer (WARP) instead of a GPU;
    // SFR_D3D11_FEATURE_LEVEL=11_0 caps the feature level (default 11_1).
    bool warp = false;
    D3D_FEATURE_LEVEL maxFeatureLevel = D3D_FEATURE_LEVEL_11_1;
    bool debug = false;
    ComPtr<IDXGIFactory2> factory;
    std::vector<std::string> deviceNames;
    RenderInterfaceCapabilities capabilities;
    D3D11Interface();
    std::unique_ptr<RenderDevice> createDevice(const std::string& preferredDeviceName) override;
    const std::vector<std::string>& getDeviceNames() const override { return deviceNames; }
    const RenderInterfaceCapabilities& getCapabilities() const override { return capabilities; }
};

struct D3D11Buffer : RenderBuffer {
    D3D11Device* device = nullptr;
    RenderBufferDesc desc;
    // UPLOAD and READBACK: the CPU memory map() returns.
    std::vector<uint8_t> cpu;
    // The GPU copy every use but constants reads (vertices, indices, copies).
    ComPtr<ID3D11Buffer> gpu;
    // The copy bound as constants (CONSTANT, or an upload buffer read so).
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    // An upload buffer mapped now (persistently mapped rings) is copied up
    // range by range at each submission; one unmapped since it was written,
    // whole and once.
    uint32_t mapCount = 0;
    bool dirty = false;
    // Set by the renderer for a buffer it fills once, before any draw reads
    // it, and keeps mapped (its kept vertices and indices): copied up whole
    // at the first submission that reads it, never again. Copying the ranges
    // a frame's draws read from every such buffer, as from the upload ring,
    // moved ~50 MB a race frame.
    bool writeOnce = false;
    bool copiedOnce = false;
    D3D11Buffer(D3D11Device* device, const RenderBufferDesc& desc);
    void* map(uint32_t subresource, const RenderRange* readRange) override;
    void unmap(uint32_t subresource, const RenderRange* writtenRange) override;
    std::unique_ptr<RenderBufferFormattedView> createBufferFormattedView(RenderFormat format) override;
    void setName(const std::string&) override {}
    uint64_t getDeviceAddress() const override { return 0; }
    ID3D11Buffer* constantBuffer();
};

struct D3D11Texture : RenderTexture {
    D3D11Device* device = nullptr;
    RenderTextureDesc desc;
    ComPtr<ID3D11Texture2D> d3d;
    ComPtr<ID3D11ShaderResourceView> srv;  // the default view
    bool swapChain = false;
    D3D11Texture() = default;
    D3D11Texture(D3D11Device* device, const RenderTextureDesc& desc);
    std::unique_ptr<RenderTextureView> createTextureView(const RenderTextureViewDesc& desc) const override;
    void setName(const std::string&) override {}
};

struct D3D11TextureView : RenderTextureView {
    ComPtr<ID3D11ShaderResourceView> srv;
};

struct D3D11Shader : RenderShader {
    // DXBC, or HLSL text compiled per pipeline.
    std::vector<uint8_t> code;
    std::string entryPoint;
    bool source = false;
    void setName(const std::string&) override {}
};

struct D3D11Sampler : RenderSampler {
    ComPtr<ID3D11SamplerState> d3d;
};

struct D3D11PipelineLayout : RenderPipelineLayout {
    std::vector<RenderPushConstantRange> pushConstantRanges;
    std::vector<RenderRootDescriptorDesc> rootDescriptors;
    // The bytes the shaders read through each root descriptor, when the
    // renderer says (rootDescriptorBytes[i] for rootDescriptors[i]): without
    // constant buffer offsets each draw copies that much, not the 64 KB
    // window D3D11.1 would bind.
    std::vector<uint32_t> rootDescriptorBytes;
    std::vector<ComPtr<ID3D11Buffer>> pushConstantBuffers;
};

struct D3D11GraphicsPipeline : RenderPipeline {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11GeometryShader> gs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> inputLayout;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depthStencil;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    uint32_t stencilReference = 0;
    std::vector<RenderInputSlot> inputSlots;
    bool valid = false;
    void setName(const std::string&) override {}
    RenderPipelineProgram getProgram(const std::string&) const override { return {}; }
};

struct D3D11DescriptorSet : RenderDescriptorSet {
    struct Entry {
        RenderDescriptorRangeType type = RenderDescriptorRangeType::UNKNOWN;
        uint32_t reg = 0;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11SamplerState> sampler;
        ComPtr<ID3D11UnorderedAccessView> uav;
        D3D11Buffer* buffer = nullptr;
    };
    std::vector<Entry> entries;
    explicit D3D11DescriptorSet(const RenderDescriptorSetDesc& desc);
    void setBuffer(uint32_t descriptorIndex, const RenderBuffer* buffer, uint64_t bufferSize,
                   const RenderBufferStructuredView* structuredView, const RenderBufferFormattedView* formattedView) override;
    void setTexture(uint32_t descriptorIndex, const RenderTexture* texture, RenderTextureLayout textureLayout,
                    const RenderTextureView* textureView) override;
    void setSampler(uint32_t descriptorIndex, const RenderSampler* sampler) override;
    void setAccelerationStructure(uint32_t, const RenderAccelerationStructure*) override {}
};

struct D3D11Framebuffer : RenderFramebuffer {
    std::vector<ComPtr<ID3D11RenderTargetView>> colors;
    ComPtr<ID3D11DepthStencilView> depth;
    uint32_t width = 0, height = 0;
    uint32_t getWidth() const override { return width; }
    uint32_t getHeight() const override { return height; }
};

struct D3D11CommandFence : RenderCommandFence {
    ComPtr<ID3D11Query> query;
    bool pending = false;
};

struct D3D11CommandSemaphore : RenderCommandSemaphore {};

// What a command list touches in upload buffers, copied up before it replays.
struct D3D11BufferUse {
    D3D11Buffer* buffer;
    uint64_t begin, end;
    bool constants;
};

// A recorded command: the frequent ones are plain data (no allocation a
// draw), anything else a callback. Payloads of varying size live in the
// list's side arrays, at the offset a command names.
enum class D3D11Op : uint8_t {
    draw,               // a vertices, b instances, c first vertex, d first instance
    drawIndexed,        // a indices, b instances, c first index, d first instance, e base vertex
    pipeline,           // p
    stencilReference,   // a
    pushConstants,      // p buffer, a register, b offset into bytes, c bytes
    descriptorSet,      // p
    xenosTextures,      // p textures, q samplers, a offset into slots
    rootDescriptor,     // p D3D11Buffer, a register, b first constant, c constants
    indexBuffer,        // p D3D11Buffer or null, a offset, b DXGI format
    vertexBuffers,      // a first slot, b count, c offset into vertexBindings
    viewports,          // a offset into viewportData, b count
    scissors,           // a offset into scissorData, b count
    framebuffer,        // p
    callback,           // a index into callbacks
};

struct D3D11Command {
    D3D11Op op;
    uint32_t a = 0, b = 0, c = 0, d = 0;
    int32_t e = 0;
    const void* p = nullptr;
    const void* q = nullptr;
};

struct D3D11VertexBinding {
    D3D11Buffer* buffer;
    UINT stride, offset;
};

struct D3D11CommandList : RenderCommandList {
    D3D11CommandQueue* queue = nullptr;
    std::vector<D3D11Command> commands;
    std::vector<uint8_t> bytes;
    std::vector<uint16_t> slots;
    std::vector<D3D11VertexBinding> vertexBindings;
    std::vector<D3D11_VIEWPORT> viewportData;
    std::vector<D3D11_RECT> scissorData;
    std::vector<std::function<void(D3D11Replay&)>> callbacks;
    std::vector<D3D11BufferUse> uses;
    // The layout recorded with, for root descriptors and push constants.
    const D3D11PipelineLayout* recordingLayout = nullptr;
    void callback(std::function<void(D3D11Replay&)> function);
    explicit D3D11CommandList(D3D11CommandQueue* queue) : queue(queue) {}
    void begin() override;
    void end() override {}
    void barriers(RenderBarrierStages, const RenderBufferBarrier*, uint32_t, const RenderTextureBarrier*, uint32_t) override {}
    void dispatch(uint32_t, uint32_t, uint32_t) override {}
    void traceRays(uint32_t, uint32_t, uint32_t, RenderBufferReference, const RenderShaderBindingGroupsInfo&) override {}
    void drawInstanced(uint32_t vertexCountPerInstance, uint32_t instanceCount, uint32_t startVertexLocation,
                       uint32_t startInstanceLocation) override;
    void drawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount, uint32_t startIndexLocation,
                              int32_t baseVertexLocation, uint32_t startInstanceLocation) override;
    void setPipeline(const RenderPipeline* pipeline) override;
    void setComputePipelineLayout(const RenderPipelineLayout*) override {}
    void setComputePushConstants(uint32_t, const void*, uint32_t, uint32_t) override {}
    void setComputeDescriptorSet(RenderDescriptorSet*, uint32_t) override {}
    void setGraphicsPipelineLayout(const RenderPipelineLayout* pipelineLayout) override;
    void setGraphicsPushConstants(uint32_t rangeIndex, const void* data, uint32_t offset, uint32_t size) override;
    void setGraphicsDescriptorSet(RenderDescriptorSet* descriptorSet, uint32_t setIndex) override;
    void setGraphicsRootDescriptor(RenderBufferReference bufferReference, uint32_t rootDescriptorIndex) override;
    void setRaytracingPipelineLayout(const RenderPipelineLayout*) override {}
    void setRaytracingPushConstants(uint32_t, const void*, uint32_t, uint32_t) override {}
    void setRaytracingDescriptorSet(RenderDescriptorSet*, uint32_t) override {}
    void setIndexBuffer(const RenderIndexBufferView* view) override;
    void setVertexBuffers(uint32_t startSlot, const RenderVertexBufferView* views, uint32_t viewCount,
                          const RenderInputSlot* inputSlots) override;
    void setViewports(const RenderViewport* viewports, uint32_t count) override;
    void setScissors(const RenderRect* scissorRects, uint32_t count) override;
    void setFramebuffer(const RenderFramebuffer* framebuffer) override;
    void setDepthBias(float, float, float) override {}
    void clearColor(uint32_t attachmentIndex, RenderColor colorValue, const RenderRect* clearRects,
                    uint32_t clearRectsCount) override;
    void clearDepthStencil(bool clearDepth, bool clearStencil, float depthValue, uint32_t stencilValue,
                           const RenderRect* clearRects, uint32_t clearRectsCount) override;
    void copyBufferRegion(RenderBufferReference dstBuffer, RenderBufferReference srcBuffer, uint64_t size) override;
    void copyTextureRegion(const RenderTextureCopyLocation& dstLocation, const RenderTextureCopyLocation& srcLocation,
                           uint32_t dstX, uint32_t dstY, uint32_t dstZ, const RenderBox* srcBox) override;
    void copyBuffer(const RenderBuffer* dstBuffer, const RenderBuffer* srcBuffer) override;
    void copyTexture(const RenderTexture* dstTexture, const RenderTexture* srcTexture) override;
    void resolveTexture(const RenderTexture* dstTexture, const RenderTexture* srcTexture) override;
    void resolveTextureRegion(const RenderTexture* dstTexture, uint32_t dstX, uint32_t dstY, const RenderTexture* srcTexture,
                              const RenderRect* srcRect, RenderResolveMode resolveMode) override;
    void buildBottomLevelAS(const RenderAccelerationStructure*, RenderBufferReference, const RenderBottomLevelASBuildInfo&) override {}
    void buildTopLevelAS(const RenderAccelerationStructure*, RenderBufferReference, RenderBufferReference,
                         const RenderTopLevelASBuildInfo&) override {}
    void discardTexture(const RenderTexture*) override {}
    void resetQueryPool(const RenderQueryPool*, uint32_t, uint32_t) override {}
    void writeTimestamp(const RenderQueryPool*, uint32_t) override {}
    // The game's Xenos draws: element s of the 2D, 2D array and cube texture
    // arrays (t0, t16, t32) and of the samplers (s0) are what entries
    // indices[s], indices[16 + s], indices[32 + s] of textures and
    // indices[48 + s] of samplers hold, for the vertex and pixel stages.
    void setXenosTextures(const RenderDescriptorSet* textures, const RenderDescriptorSet* samplers,
                          const std::array<uint32_t, 64>& indices);
    void setStencilReference(uint32_t reference);
};

struct D3D11SwapChain : RenderSwapChain {
    D3D11CommandQueue* queue = nullptr;
    ComPtr<IDXGISwapChain1> d3d;
    RenderWindow window{};
    RenderFormat format = RenderFormat::UNKNOWN;
    uint32_t textureCount = 0, width = 0, height = 0, nextIndex = 0;
    bool vsync = true, tearing = false;
    // Flip model in D3D11: buffer 0 is always the one to draw into, so every
    // index the presentation acquires wraps it.
    std::vector<std::unique_ptr<D3D11Texture>> textures;
    D3D11SwapChain(D3D11CommandQueue* queue, RenderWindow window, uint32_t textureCount, RenderFormat format,
                   uint32_t maxFrameLatency);
    bool present(uint32_t textureIndex, RenderCommandSemaphore** waitSemaphores, uint32_t waitSemaphoreCount) override;
    void wait() override {}
    bool resize() override;
    bool needsResize() const override;
    void setVsyncEnabled(bool vsyncEnabled) override { vsync = vsyncEnabled; }
    bool isVsyncEnabled() const override { return vsync; }
    uint32_t getWidth() const override { return width; }
    uint32_t getHeight() const override { return height; }
    RenderTexture* getTexture(uint32_t textureIndex) override { return textures[textureIndex].get(); }
    uint32_t getTextureCount() const override { return textureCount; }
    bool acquireTexture(RenderCommandSemaphore* signalSemaphore, uint32_t* textureIndex) override;
    RenderWindow getWindow() const override { return window; }
    bool isEmpty() const override { return d3d == nullptr || width == 0 || height == 0; }
    uint32_t getRefreshRate() const override { return 0; }
    void wrapBackBuffer();
};

struct D3D11CommandQueue : RenderCommandQueue {
    D3D11Device* device = nullptr;
    explicit D3D11CommandQueue(D3D11Device* device) : device(device) {}
    std::unique_ptr<RenderCommandList> createCommandList() override;
    std::unique_ptr<RenderSwapChain> createSwapChain(RenderWindow renderWindow, uint32_t textureCount, RenderFormat format,
                                                     uint32_t maxFrameLatency) override;
    void executeCommandLists(const RenderCommandList** commandLists, uint32_t commandListCount,
                             RenderCommandSemaphore** waitSemaphores, uint32_t waitSemaphoreCount,
                             RenderCommandSemaphore** signalSemaphores, uint32_t signalSemaphoreCount,
                             RenderCommandFence* signalFence) override;
    void waitForCommandFence(RenderCommandFence* fence) override;
};

// One submission's cost, for NATIVE_D3D11_REPLAY.
struct D3D11ReplayStats {
    float milliseconds, uploadMilliseconds;
    uint32_t draws, calls, skipped;
    uint64_t uploadBytes;
};

struct D3D11Device : RenderDevice {
    D3D11Interface* renderInterface = nullptr;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID3D11Device1> d3d1;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext1> context1;
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    // D3D11.1's *SetConstantBuffers1 with offsets. Without them (an old
    // driver, or SFR_D3D11_NO_CB_OFFSET=1) a root descriptor's window is
    // copied at the draw into windowConstants[register], a dynamic constant
    // buffer mapped with WRITE_DISCARD, and bound whole.
    bool constantBufferOffsets = false;
    std::array<ComPtr<ID3D11Buffer>, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> windowConstants;
    std::array<uint32_t, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> windowConstantBytes{};
    ID3D11Buffer* windowConstantBuffer(UINT reg, uint32_t bytes);
    // The immediate context is one thread's at a time.
    std::recursive_mutex contextMutex;
    RenderDeviceCapabilities capabilities;
    RenderDeviceDescription description;
    // Clearing part of a depth target: a draw of its own.
    ComPtr<ID3D11VertexShader> clearVertexShader;
    ComPtr<ID3D11Buffer> clearConstants;
    ComPtr<ID3D11RasterizerState> clearRasterizer;
    ComPtr<ID3D11BlendState> clearBlend;
    // Clearing part of a colour target where the driver lacks ClearView
    // (D3D11_OPTIONS::ClearView, false wherever constant buffer offsets
    // are): the same triangle, writing the colour into that target only.
    bool clearView = false;
    ComPtr<ID3D11PixelShader> clearPixelShader;
    std::array<ComPtr<ID3D11BlendState>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> clearColorBlends;
    std::unordered_map<uint32_t, ComPtr<ID3D11DepthStencilState>> clearDepthStates;
    // Compiled shaders by source hash, entry, profile and specialization:
    // done, or being compiled (by a pipeline or the prewarm threads), so
    // that one is never compiled twice at once.
    std::mutex shaderMutex;
    std::unordered_map<std::string, std::shared_future<std::vector<uint8_t>>> compiledShaders;
    // Prewarm (SFR_D3D11_PREWARM=0 turns it off): HLSL shaders start
    // compiling on threads of their own when the game makes them (as a
    // level loads), at the specialization most pipelines use, so that the
    // first draw with them seldom waits for d3dcompiler.
    bool prewarm = true;
    std::mutex prewarmMutex;
    std::condition_variable prewarmWake;
    std::deque<std::function<void()>> prewarmJobs;
    std::vector<std::thread> prewarmThreads;
    bool prewarmStop = false;
    void prewarmShader(const D3D11Shader& shader);
    // A submission's upload ranges (reused), under contextMutex.
    std::vector<D3D11BufferUse> uploadUses;
    // Replay timing (NATIVE_D3D11_REPLAY), under contextMutex.
    std::vector<D3D11ReplayStats> replayStats;
    void noteReplay(const D3D11ReplayStats& stats);

    D3D11Device(D3D11Interface* renderInterface, ComPtr<IDXGIAdapter1> adapter);
    ~D3D11Device() override;
    bool valid() const { return d3d != nullptr; }
    std::unique_ptr<RenderDescriptorSet> createDescriptorSet(const RenderDescriptorSetDesc& desc) override;
    std::unique_ptr<RenderShader> createShader(const void* data, uint64_t size, const char* entryPointName,
                                               RenderShaderFormat format) override;
    std::unique_ptr<RenderSampler> createSampler(const RenderSamplerDesc& desc) override;
    std::unique_ptr<RenderPipeline> createComputePipeline(const RenderComputePipelineDesc&) override { return nullptr; }
    std::unique_ptr<RenderPipeline> createGraphicsPipeline(const RenderGraphicsPipelineDesc& desc) override;
    std::unique_ptr<RenderPipeline> createRaytracingPipeline(const RenderRaytracingPipelineDesc&, const RenderPipeline*) override {
        return nullptr;
    }
    std::unique_ptr<RenderCommandQueue> createCommandQueue(RenderCommandListType type) override;
    std::unique_ptr<RenderBuffer> createBuffer(const RenderBufferDesc& desc) override;
    std::unique_ptr<RenderTexture> createTexture(const RenderTextureDesc& desc) override;
    std::unique_ptr<RenderAccelerationStructure> createAccelerationStructure(const RenderAccelerationStructureDesc&) override {
        return nullptr;
    }
    std::unique_ptr<RenderPool> createPool(const RenderPoolDesc& desc) override;
    std::unique_ptr<RenderPipelineLayout> createPipelineLayout(const RenderPipelineLayoutDesc& desc) override;
    std::unique_ptr<RenderCommandFence> createCommandFence() override;
    std::unique_ptr<RenderCommandSemaphore> createCommandSemaphore() override;
    std::unique_ptr<RenderFramebuffer> createFramebuffer(const RenderFramebufferDesc& desc) override;
    std::unique_ptr<RenderQueryPool> createQueryPool(uint32_t) override { return nullptr; }
    void setBottomLevelASBuildInfo(RenderBottomLevelASBuildInfo&, const RenderBottomLevelASMesh*, uint32_t, bool, bool) override {}
    void setTopLevelASBuildInfo(RenderTopLevelASBuildInfo&, const RenderTopLevelASInstance*, uint32_t, bool, bool) override {}
    void setShaderBindingTableInfo(RenderShaderBindingTableInfo&, const RenderShaderBindingGroups&, const RenderPipeline*,
                                   RenderDescriptorSet**, uint32_t) override {}
    const RenderDeviceCapabilities& getCapabilities() const override { return capabilities; }
    const RenderDeviceDescription& getDescription() const override { return description; }
    RenderSampleCounts getSampleCountsSupported(RenderFormat) const override { return RenderSampleCount::COUNT_1; }
    bool beginCapture() override { return false; }
    bool endCapture() override { return false; }
    // The DXBC for a shader at this specialization (compiled once); empty,
    // with the compiler's message logged, when it does not compile.
    std::vector<uint8_t> shaderCode(const D3D11Shader* shader, const char* profile, uint32_t specConstants);
    std::vector<uint8_t> compiledCode(const std::vector<uint8_t>& source, const std::string& entry, const char* profile,
                                      uint32_t specConstants, bool prewarming);
};

std::unique_ptr<RenderInterface> CreateD3D11Interface();

// The key a compiled shader is kept under, on disk and in shaders-d3d11.pack:
// the HLSL's hash and size, entry point, profile and specialization.
std::string D3D11ShaderKey(std::span<const uint8_t> source, const std::string& entry, const char* profile,
                           uint32_t specConstants);
// d3dcompiler_47 as the backend compiles (optimization level 3, the
// specialization as SFR_SPEC_CONSTANTS); empty with errors when it fails.
std::vector<uint8_t> D3D11CompileShader(std::span<const uint8_t> source, const std::string& entry, const char* profile,
                                        uint32_t specConstants, std::string* errors);
// Shaders compiled ahead of time, by D3D11ShaderKey (shaders-d3d11.pack,
// runtime_shader_cache.cpp sets it): null when the pack has none.
extern const std::vector<uint8_t>* (*D3D11PrecompiledShader)(const std::string& key);
}
#endif
