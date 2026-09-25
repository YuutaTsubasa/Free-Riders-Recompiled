#include "native_pipeline_key.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
    try {
        sfr::NativeDraw draw;
        std::vector<uint8_t> legacy, bulk;
        const auto compare = [&] {
            sfr::native_pipeline_key_legacy(draw, legacy);
            sfr::native_pipeline_key_bulk(draw, bulk);
            require(!legacy.empty() && legacy == bulk, "bulk key matches existing pipeline identity byte for byte");
        };
        compare();
        // Reuse the buffers across growth/shrink and stencil transitions.
        for (unsigned count : {1u, 20u, 36u, 0u, 20u}) {
            draw.elements.clear();
            for (unsigned i = 0; i < count; ++i)
                draw.elements.emplace_back(i & 1 ? "TEXCOORD" : "POSITION", i,
                    i, plume::RenderFormat::R32_FLOAT, i % 2, i * 4);
            for (unsigned n = 0; n < 128; ++n) {
                draw.pixel_spec_constants = n * 7919;
                draw.stride = n * 4;
                draw.stencil_enabled = n & 1;
                draw.stencil_reference = n;
                draw.stencil_read_mask = n ^ 0xFF;
                draw.stencil_write_mask = n ^ 0xA5;
                draw.depth_enabled = n & 2;
                draw.depth_write = n & 4;
                draw.write_mask = n & 15;
                draw.blend.blendEnabled = n & 8;
                draw.depth_function = n & 16 ? plume::RenderComparisonFunction::LESS_EQUAL
                                            : plume::RenderComparisonFunction::ALWAYS;
                compare();
            }
        }
        const auto baseline = bulk;
        draw.vertex_count = 777;
        draw.base_vertex_location = 31;
        draw.vertex_constants[0] = 0xFFFFFFFF;
        draw.shared.alpha_threshold = 0.5f;
        compare();
        require(bulk == baseline, "per-draw data is not part of the pipeline identity");
        draw.stencil_enabled = false;
        compare();
        const auto no_stencil = bulk;
        draw.stencil_reference ^= 0xFF;
        draw.stencil_read_mask ^= 0xFF;
        compare();
        require(bulk == no_stencil, "disabled stencil fields do not split pipelines");
        draw.stencil_enabled = true;
        // Use valid fields with deliberately different padding, as the old key
        // includes the object representation of blend and stencil descriptors.
        for (unsigned char pattern : {0u, 0x55u, 0xAAu}) {
            std::memset(&draw.blend, pattern, sizeof(draw.blend));
            draw.blend.srcBlend = plume::RenderBlend::ONE;
            draw.blend.dstBlend = plume::RenderBlend::ZERO;
            draw.blend.blendOp = plume::RenderBlendOperation::ADD;
            draw.blend.srcBlendAlpha = plume::RenderBlend::ONE;
            draw.blend.dstBlendAlpha = plume::RenderBlend::ZERO;
            draw.blend.blendOpAlpha = plume::RenderBlendOperation::ADD;
            draw.blend.blendEnabled = true;
            compare();
        }
        const auto changes_identity = [&](auto change) {
            compare();
            const auto before = bulk;
            change();
            compare();
            require(before != bulk, "a keyed state change produces a different pipeline identity");
        };
        int shader_tokens[2]; // Identity only; the serializer never dereferences shaders.
        changes_identity([&] { draw.vertex_shader = reinterpret_cast<const plume::RenderShader*>(&shader_tokens[0]); });
        changes_identity([&] { draw.pixel_shader = reinterpret_cast<const plume::RenderShader*>(&shader_tokens[1]); });
        changes_identity([&] { draw.topology = plume::RenderPrimitiveTopology::LINE_LIST; });
        changes_identity([&] { draw.cull = plume::RenderCullMode::BACK; });
        changes_identity([&] { draw.blend.dstBlend = plume::RenderBlend::ONE; });
        changes_identity([&] { draw.stencil_front.passOp = plume::RenderStencilOp::REPLACE; });
        changes_identity([&] { draw.stencil_back.compareFunction = plume::RenderComparisonFunction::LESS_EQUAL; });
        changes_identity([&] { draw.elements[0].semanticIndex += 1; });
        changes_identity([&] { draw.elements[0].format = plume::RenderFormat::R32G32_FLOAT; });
        changes_identity([&] { draw.elements[0].slotIndex += 1; });
        changes_identity([&] { draw.elements[0].alignedByteOffset += 4; });
        changes_identity([&] { draw.elements[0].semanticName = "NORMAL"; });
        if (argc > 1 && std::string_view(argv[1]) == "--bench") {
            // Diagnostic timing only; it never decides whether the test passes.
            uint64_t checksum = 0;
            for (bool fast : {false, true, true, false}) {
                const auto start = std::chrono::steady_clock::now();
                for (unsigned i = 0; i < 500000; ++i) {
                    draw.pixel_spec_constants = i;
                    if (fast) sfr::native_pipeline_key_bulk(draw, bulk);
                    else sfr::native_pipeline_key_legacy(draw, bulk);
                    checksum += bulk[i % bulk.size()];
                }
                std::cout << (fast ? "bulk" : "legacy") << " ms="
                    << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()
                    << " checksum=" << checksum << '\n';
            }
        }
        std::cout << "native pipeline key regression passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
