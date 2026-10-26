#include "screen_overlay_renderer.h"
#include "plume_render_interface_builders.h"
#include <algorithm>
#include <cstring>
#include <deque>
#include <iostream>

#include "overlay_vs_dxil.h"
#include "overlay_ps_dxil.h"
#include "overlay_vs_spirv.h"
#include "overlay_ps_spirv.h"

namespace sfr {
struct ScreenOverlayRenderer::Impl {
    plume::RenderDevice& device;
    plume::RenderCommandQueue& queue;
    GraphicsBackend backend;
    plume::RenderFormat target_format;
    bool failed = false;
    std::unique_ptr<plume::RenderShader> vertex, pixel;
    std::unique_ptr<plume::RenderPipelineLayout> layout;
    std::unique_ptr<plume::RenderPipeline> pipeline;
    std::unique_ptr<plume::RenderDescriptorSet> sampler_set;
    std::unique_ptr<plume::RenderSampler> sampler;
    plume::RenderDescriptorSetBuilder textures;
    struct Picture {
        uint64_t id = 0;
        uint64_t used = 0;  // the frame that last drew it
        std::unique_ptr<plume::RenderTexture> texture;
        std::unique_ptr<plume::RenderTextureView> view;
        std::unique_ptr<plume::RenderDescriptorSet> set;
    };
    // The newest last. Beyond `kept`, the oldest go once no frame that may
    // still be in flight (three can be) has drawn them.
    std::deque<Picture> pictures;
    static constexpr size_t kept = 4;
    uint64_t frame = 0;

    Impl(plume::RenderDevice& d, plume::RenderCommandQueue& q, GraphicsBackend b, plume::RenderFormat f)
        : device(d), queue(q), backend(b), target_format(f) {}

    bool build() {
        if (pipeline) return true;
        if (failed) return false;
        failed = true;  // until everything below is made
        if (backend == GraphicsBackend::vulkan) {
            vertex = device.createShader(overlay_vs_spirv, sizeof(overlay_vs_spirv), "vertexMain", plume::RenderShaderFormat::SPIRV);
            pixel = device.createShader(overlay_ps_spirv, sizeof(overlay_ps_spirv), "pixelMain", plume::RenderShaderFormat::SPIRV);
        } else {
            vertex = device.createShader(overlay_vs_dxil, sizeof(overlay_vs_dxil), "vertexMain", plume::RenderShaderFormat::DXIL);
            pixel = device.createShader(overlay_ps_dxil, sizeof(overlay_ps_dxil), "pixelMain", plume::RenderShaderFormat::DXIL);
        }
        if (!vertex || !pixel) return false;
        plume::RenderPipelineLayoutBuilder builder;
        builder.begin(false, false);
        textures.begin();
        textures.addTexture(0);
        textures.end();
        builder.addDescriptorSet(textures);
        plume::RenderDescriptorSetBuilder samplers;
        samplers.begin();
        samplers.addSampler(0);
        samplers.end();
        sampler_set = samplers.create(&device);
        builder.addDescriptorSet(samplers);
        builder.addPushConstant(0, 2, sizeof(float) * 8, plume::RenderShaderStageFlag::VERTEX | plume::RenderShaderStageFlag::PIXEL);
        builder.end();
        layout = builder.create(&device);
        if (!layout || !sampler_set) return false;
        plume::RenderSamplerDesc sampler_desc;
        sampler_desc.minFilter = plume::RenderFilter::LINEAR;
        sampler_desc.magFilter = plume::RenderFilter::LINEAR;
        sampler_desc.addressU = plume::RenderTextureAddressMode::CLAMP;
        sampler_desc.addressV = plume::RenderTextureAddressMode::CLAMP;
        sampler_desc.addressW = plume::RenderTextureAddressMode::CLAMP;
        sampler = device.createSampler(sampler_desc);
        if (!sampler) return false;
        sampler_set->setSampler(0, sampler.get());
        plume::RenderGraphicsPipelineDesc desc;
        desc.pipelineLayout = layout.get();
        desc.vertexShader = vertex.get();
        desc.pixelShader = pixel.get();
        desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
        desc.cullMode = plume::RenderCullMode::NONE;
        desc.depthEnabled = false;
        desc.depthWriteEnabled = false;
        desc.renderTargetCount = 1;
        desc.renderTargetFormat[0] = target_format;
        // Premultiplied alpha: the colour is already multiplied by it.
        plume::RenderBlendDesc blend = plume::RenderBlendDesc::AlphaBlend();
        blend.srcBlend = plume::RenderBlend::ONE;
        desc.renderTargetBlend[0] = blend;
        pipeline = device.createGraphicsPipeline(desc);
        if (!pipeline) return false;
        failed = false;
        return true;
    }

    const Picture* find(uint64_t id) const {
        for (const auto& picture : pictures)
            if (picture.id == id) return &picture;
        return nullptr;
    }

    bool upload(const OverlayImage& image) {
        Picture picture;
        picture.id = image.id;
        picture.texture = device.createTexture(
            plume::RenderTextureDesc::Texture2D(image.width, image.height, 1, plume::RenderFormat::R8G8B8A8_UNORM));
        const uint32_t row_bytes = (image.width * 4 + 255) & ~255u;  // rows of a copy start on 256 bytes
        auto staging = device.createBuffer(plume::RenderBufferDesc::UploadBuffer(uint64_t(row_bytes) * image.height));
        auto list = queue.createCommandList();
        auto fence = device.createCommandFence();
        if (!picture.texture || !staging || !list || !fence) return false;
        auto* const at = static_cast<uint8_t*>(staging->map());
        if (!at) return false;
        for (uint32_t row = 0; row < image.height; ++row)
            std::memcpy(at + uint64_t(row) * row_bytes, image.rgba.data() + size_t(row) * image.width * 4, size_t(image.width) * 4);
        staging->unmap();
        list->begin();
        list->barriers(plume::RenderBarrierStage::COPY,
                       plume::RenderTextureBarrier(picture.texture.get(), plume::RenderTextureLayout::COPY_DEST));
        list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(picture.texture.get()),
                                plume::RenderTextureCopyLocation::PlacedFootprint(staging.get(), plume::RenderFormat::R8G8B8A8_UNORM,
                                                                                  image.width, image.height, 1, row_bytes / 4));
        list->barriers(plume::RenderBarrierStage::GRAPHICS,
                       plume::RenderTextureBarrier(picture.texture.get(), plume::RenderTextureLayout::SHADER_READ));
        list->end();
        queue.executeCommandLists(list.get(), fence.get());
        queue.waitForCommandFence(fence.get());
        picture.view = picture.texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R8G8B8A8_UNORM));
        picture.set = textures.create(&device);
        if (!picture.view || !picture.set) return false;
        picture.set->setTexture(0, picture.texture.get(), plume::RenderTextureLayout::SHADER_READ, picture.view.get());
        picture.used = frame;
        pictures.push_back(std::move(picture));
        for (auto it = pictures.begin(); pictures.size() > kept && it != pictures.end();)
            it = it->used + 4 < frame ? pictures.erase(it) : std::next(it);
        return true;
    }
};

ScreenOverlayRenderer::ScreenOverlayRenderer(plume::RenderDevice& device, plume::RenderCommandQueue& queue, GraphicsBackend backend,
                                             plume::RenderFormat target_format)
    : impl_(std::make_unique<Impl>(device, queue, backend, target_format)) {}
ScreenOverlayRenderer::~ScreenOverlayRenderer() = default;

bool ScreenOverlayRenderer::prepare(const std::vector<ScreenLayer>& layers) {
    if (layers.empty()) return true;
    if (!impl_->build()) return false;
    for (const ScreenLayer& layer : layers) {
        const OverlayImage* image = layer.image.get();
        if (!image || !image->width || !image->height || impl_->find(image->id)) continue;
        if (!impl_->upload(*image)) {
            static bool told = false;
            if (!told) std::cerr << "SCREEN_OVERLAY upload_failed width=" << image->width << " height=" << image->height << '\n';
            told = true;
            return false;
        }
    }
    return true;
}

void ScreenOverlayRenderer::draw(plume::RenderCommandList& list, const std::vector<ScreenLayer>& layers, float view_x,
                                 float view_y, float view_width, float view_height, float out_width, float out_height) {
    ++impl_->frame;
    if (layers.empty() || !impl_->pipeline || out_width <= 0 || out_height <= 0) return;
    const plume::RenderViewport viewport(0.0f, 0.0f, out_width, out_height);
    const plume::RenderRect scissor(0, 0, int32_t(out_width), int32_t(out_height));
    list.setViewports(&viewport, 1);
    list.setScissors(&scissor, 1);
    list.setGraphicsPipelineLayout(impl_->layout.get());
    list.setPipeline(impl_->pipeline.get());
    list.setGraphicsDescriptorSet(impl_->sampler_set.get(), 1);
    for (const ScreenLayer& layer : layers) {
        const OverlayImage* image = layer.image.get();
        const Impl::Picture* picture = image ? impl_->find(image->id) : nullptr;
        if (!picture || layer.opacity <= 0.0f || view_width <= 0 || view_height <= 0) continue;
        // Window pixels, then clip space (y up).
        const float left = view_x + layer.x, top = view_y + layer.y;
        const float constants[8] = {left / out_width * 2.0f - 1.0f, 1.0f - top / out_height * 2.0f,
                                    (left + layer.width) / out_width * 2.0f - 1.0f,
                                    1.0f - (top + layer.height) / out_height * 2.0f,
                                    std::clamp(layer.opacity, 0.0f, 1.0f), 0.0f, 0.0f, 0.0f};
        const_cast<Impl::Picture*>(picture)->used = impl_->frame;
        list.setGraphicsDescriptorSet(picture->set.get(), 0);
        list.setGraphicsPushConstants(0, constants);
        list.drawInstanced(6, 1, 0, 0);
    }
}
}
