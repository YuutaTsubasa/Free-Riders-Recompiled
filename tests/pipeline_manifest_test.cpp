#include "pipeline_manifest.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<class F> static void rejects(F operation, const char* message) {
    try { operation(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(message);
}

static sfr::PipelineRecipe sample() {
    sfr::PipelineRecipe recipe;
    recipe.vertex = {0x123456789abcdef0ull, 64};
    recipe.pixel = {0xfedcba9876543210ull, 96};
    recipe.pixel_link_constants = 7;
    auto& d = recipe.state;
    d.pixel_spec_constants = 9;
    d.stride = 20;
    d.elements.emplace_back("POSITION", 0, 0, plume::RenderFormat::R32G32B32_FLOAT, 0, 0);
    d.elements.emplace_back("TEXCOORD", 1, 1, plume::RenderFormat::R32G32_FLOAT, 0, 12);
    d.elements.emplace_back("NORMAL", 0, 2, plume::RenderFormat::R32G32B32_FLOAT, 15, 0);
    d.blend.blendEnabled = true;
    d.blend.srcBlend = plume::RenderBlend::SRC_ALPHA;
    d.blend.dstBlend = plume::RenderBlend::INV_SRC_ALPHA;
    d.write_mask = 7;
    d.depth_enabled = d.depth_write = true;
    d.depth_function = plume::RenderComparisonFunction::LESS;
    d.cull = plume::RenderCullMode::BACK;
    d.stencil_enabled = true;
    d.stencil_reference = 3;
    d.stencil_read_mask = 0x7f;
    d.stencil_write_mask = 0xf0;
    d.stencil_front.passOp = plume::RenderStencilOp::REPLACE;
    d.stencil_back.depthFailOp = plume::RenderStencilOp::INCREMENT_AND_WRAP;
    return recipe;
}

static void put32(std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(at + i) = uint8_t(value >> (8 * i));
}
static void repair_checksum(std::vector<uint8_t>& bytes) {
    const auto hash = sfr::pipeline_shader_id(std::span(bytes).first(bytes.size() - 8)).hash;
    for (unsigned i = 0; i < 8; ++i) bytes.at(bytes.size() - 8 + i) = uint8_t(hash >> (8 * i));
}

int main() {
    try {
        const uint8_t source[]{'h', 'e', 'l', 'l', 'o'};
        const auto id = sfr::pipeline_shader_id(source);
        require(id.hash == 0xa430d84680aabd0bull && id.size == 5,
                "shader identity uses standard FNV-1a64 and source length");
        auto recipe = sample();
        const auto baseline = sfr::pipeline_recipe_key(recipe);
        require(!baseline.empty(), "recipe key encodes state");
        require(baseline[0] == 0xf0 && baseline[7] == 0x12 && baseline[8] == 64,
                "recipe scalar encoding is explicitly little endian");
        const auto one = std::span<const sfr::PipelineRecipe>(&recipe, 1);
        for (uint32_t backend : {0u, 1u}) {
            const auto bytes = sfr::encode_pipeline_manifest(one, backend);
            auto decoded = sfr::decode_pipeline_manifest(bytes, backend);
            require(decoded.size() == 1 && sfr::pipeline_recipe_key(decoded.front()) == baseline,
                    "all graphics state and shader identities round trip");
            require(!decoded[0].state.vertex_shader && !decoded[0].state.pixel_shader &&
                    decoded[0].state.vertices.empty() && decoded[0].state.indices.empty(),
                    "decoded recipe has no process-local draw references");
            require(std::string(decoded[0].state.elements[0].semanticName) == "POSITION",
                    "semantic name survives decoding");
            rejects([&] { sfr::decode_pipeline_manifest(bytes, 1 - backend); }, "backend mismatch rejected");
            for (size_t cut = 0; cut < bytes.size(); ++cut)
                rejects([&] { sfr::decode_pipeline_manifest(std::span(bytes).first(cut), backend); },
                        "every truncated prefix rejected");
            for (size_t pos = 0; pos < bytes.size(); ++pos) {
                auto corrupt = bytes;
                corrupt[pos] ^= 1;
                rejects([&] { sfr::decode_pipeline_manifest(corrupt, backend); }, "single-byte corruption rejected");
            }
            auto tail = bytes;
            tail.push_back(0);
            rejects([&] { sfr::decode_pipeline_manifest(tail, backend); }, "trailing bytes rejected");
            // These mutations have valid checksums, so exercise structural and
            // state validation rather than merely checksum rejection.
            const auto bad_word = [&](size_t at, uint32_t value) {
                auto corrupt = bytes;
                put32(corrupt, at, value);
                repair_checksum(corrupt);
                rejects([&] { sfr::decode_pipeline_manifest(corrupt, backend); }, "invalid manifest field rejected");
            };
            bad_word(8, 99); // format version
            bad_word(12, 0); // shader ABI
            bad_word(20, uint32_t(sfr::pipeline_manifest_max_recipes + 1));
            bad_word(32, 0xffffffff); // record length
            constexpr size_t key_start = 36;
            bad_word(key_start + 8, 0); // missing vertex source identity
            bad_word(key_start + 36, 99); // topology
            bad_word(key_start + 40, 0xffffffff); // element count
            bad_word(key_start + 44, 99); // semantic name
            bad_word(key_start + 44 + 12, uint32_t(plume::RenderFormat::BC1_UNORM));
            bad_word(key_start + 44 + 16, 9); // unbound input slot
            bad_word(key_start + 44 + 20, 0xffffffff); // offset
            bad_word(key_start + 44 + 3 * 24 + 24, 2); // blend boolean
            constexpr size_t blend_start = key_start + 44 + 3 * 24;
            bad_word(blend_start, 0xffffffff); // blend enum
            bad_word(blend_start + 8, 0); // blend operation UNKNOWN
            bad_word(blend_start + 28, 256); // write mask narrowing
            bad_word(blend_start + 32, 2); // depth boolean
            bad_word(blend_start + 40, 0); // depth comparison UNKNOWN
            bad_word(blend_start + 44, 0); // cull UNKNOWN
            bad_word(blend_start + 48, 2); // stencil boolean
            bad_word(blend_start + 52, 256); // stencil reference narrowing
            bad_word(blend_start + 64, 0); // stencil operation UNKNOWN
            bad_word(blend_start + 76, 0); // stencil comparison UNKNOWN
        }
        auto same = recipe;
        int shader_tokens[2];
        std::string semantic = "POSITION";
        const uint8_t vertex[]{1, 2, 3, 4};
        same.state.vertex_shader = reinterpret_cast<const plume::RenderShader*>(&shader_tokens[0]);
        same.state.pixel_shader = reinterpret_cast<const plume::RenderShader*>(&shader_tokens[1]);
        same.state.elements[0].semanticName = semantic.c_str();
        same.state.vertices = vertex;
        same.state.vertex_count = 42;
        same.state.vertex_constants[0] = 123;
        require(sfr::pipeline_recipe_key(same) == baseline, "keys ignore addresses and per-draw data");
        const sfr::PipelineRecipe duplicates[]{recipe, same, recipe};
        const auto deduped = sfr::decode_pipeline_manifest(sfr::encode_pipeline_manifest(duplicates, 0), 0);
        require(deduped.size() == 1, "duplicate keys stored once");
        auto no_stencil = recipe;
        no_stencil.state.stencil_enabled = false;
        const auto no_stencil_key = sfr::pipeline_recipe_key(no_stencil);
        no_stencil.state.stencil_reference ^= 0xff;
        no_stencil.state.stencil_front.passOp = plume::RenderStencilOp::UNKNOWN;
        require(sfr::pipeline_recipe_key(no_stencil) == no_stencil_key, "inactive stencil state is ignored");
        const auto changes_key = [&](auto mutation) {
            auto changed = recipe;
            mutation(changed);
            require(sfr::pipeline_recipe_key(changed) != baseline, "pipeline state changes key");
        };
        changes_key([](auto& r) { ++r.vertex.hash; });
        changes_key([](auto& r) { ++r.vertex.size; });
        changes_key([](auto& r) { ++r.pixel.hash; });
        changes_key([](auto& r) { ++r.pixel.size; });
        changes_key([](auto& r) { ++r.pixel_link_constants; });
        changes_key([](auto& r) { ++r.state.pixel_spec_constants; });
        changes_key([](auto& r) { r.state.topology = plume::RenderPrimitiveTopology::LINE_LIST; });
        changes_key([](auto& r) { r.state.stride += 4; });
        changes_key([](auto& r) { r.state.elements[0].semanticName = "COLOR"; });
        changes_key([](auto& r) { ++r.state.elements[0].semanticIndex; });
        changes_key([](auto& r) { r.state.elements[0].location = 3; });
        changes_key([](auto& r) { r.state.elements[0].format = plume::RenderFormat::R32G32B32A32_FLOAT; });
        changes_key([](auto& r) { r.state.elements[0].slotIndex = 15; });
        changes_key([](auto& r) { r.state.elements[0].alignedByteOffset = 4; });
        changes_key([](auto& r) { r.state.blend.srcBlend = plume::RenderBlend::ONE; });
        changes_key([](auto& r) { r.state.blend.dstBlend = plume::RenderBlend::ZERO; });
        changes_key([](auto& r) { r.state.blend.blendOp = plume::RenderBlendOperation::SUBTRACT; });
        changes_key([](auto& r) { r.state.blend.srcBlendAlpha = plume::RenderBlend::SRC_ALPHA; });
        changes_key([](auto& r) { r.state.blend.dstBlendAlpha = plume::RenderBlend::DEST_ALPHA; });
        changes_key([](auto& r) { r.state.blend.blendOpAlpha = plume::RenderBlendOperation::REV_SUBTRACT; });
        changes_key([](auto& r) { r.state.blend.blendEnabled = false; });
        changes_key([](auto& r) { r.state.write_mask = 15; });
        changes_key([](auto& r) { r.state.depth_enabled = false; });
        changes_key([](auto& r) { r.state.depth_write = false; });
        changes_key([](auto& r) { r.state.depth_function = plume::RenderComparisonFunction::ALWAYS; });
        changes_key([](auto& r) { r.state.cull = plume::RenderCullMode::FRONT; });
        changes_key([](auto& r) { r.state.stencil_enabled = false; });
        changes_key([](auto& r) { ++r.state.stencil_reference; });
        changes_key([](auto& r) { ++r.state.stencil_read_mask; });
        changes_key([](auto& r) { ++r.state.stencil_write_mask; });
        changes_key([](auto& r) { r.state.stencil_front.passOp = plume::RenderStencilOp::ZERO; });
        changes_key([](auto& r) { r.state.stencil_front.failOp = plume::RenderStencilOp::ZERO; });
        changes_key([](auto& r) { r.state.stencil_front.depthFailOp = plume::RenderStencilOp::ZERO; });
        changes_key([](auto& r) { r.state.stencil_front.compareFunction = plume::RenderComparisonFunction::LESS; });
        changes_key([](auto& r) { r.state.stencil_back.passOp = plume::RenderStencilOp::ZERO; });
        changes_key([](auto& r) { r.state.stencil_back.failOp = plume::RenderStencilOp::ZERO; });
        changes_key([](auto& r) { r.state.stencil_back.depthFailOp = plume::RenderStencilOp::ZERO; });
        changes_key([](auto& r) { r.state.stencil_back.compareFunction = plume::RenderComparisonFunction::LESS; });
        const auto invalid = [&](auto mutation) {
            auto changed = recipe;
            mutation(changed);
            rejects([&] { sfr::pipeline_recipe_key(changed); }, "invalid pipeline state rejected on encoding");
        };
        invalid([](auto& r) { r.state.topology = plume::RenderPrimitiveTopology::TRIANGLE_FAN; });
        invalid([](auto& r) { r.state.depth_function = plume::RenderComparisonFunction::UNKNOWN; });
        invalid([](auto& r) { r.state.cull = plume::RenderCullMode::UNKNOWN; });
        invalid([](auto& r) { r.state.blend.srcBlend = plume::RenderBlend::UNKNOWN; });
        invalid([](auto& r) { r.state.blend.blendOp = plume::RenderBlendOperation::UNKNOWN; });
        invalid([](auto& r) { r.state.stencil_front.passOp = plume::RenderStencilOp::UNKNOWN; });
        invalid([](auto& r) { r.state.write_mask = 16; });
        invalid([](auto& r) { r.state.elements[0].semanticName = "CUSTOM"; });
        invalid([](auto& r) { r.state.elements[0].semanticName = nullptr; });
        invalid([](auto& r) { r.state.elements[1].location = 0; });
        invalid([](auto& r) { r.state.elements[1] = r.state.elements[0]; r.state.elements[1].location = 1; });
        invalid([](auto& r) { r.state.elements.resize(sfr::pipeline_manifest_max_elements + 1); });
        invalid([](auto& r) { r.state.stride = 0; });
        invalid([](auto& r) { r.state.elements[0].location = 32; });
        invalid([](auto& r) { r.state.elements[0].format = plume::RenderFormat::R32G32B32A32_TYPELESS; });
        invalid([](auto& r) { r.state.elements[0].format = plume::RenderFormat::D32_FLOAT; });
        invalid([](auto& r) { r.state.blend.srcBlend = plume::RenderBlend::SRC1_COLOR; });
        invalid([](auto& r) { r.state.blend.srcBlendAlpha = plume::RenderBlend::SRC_COLOR; });
        auto vertex_id = recipe;
        vertex_id.state.elements.clear();
        vertex_id.state.stride = 0;
        const auto vertex_id_bytes = sfr::encode_pipeline_manifest(std::span(&vertex_id, 1), 0);
        require(sfr::pipeline_recipe_key(sfr::decode_pipeline_manifest(vertex_id_bytes, 0).at(0)) ==
                sfr::pipeline_recipe_key(vertex_id), "SV_VertexID pipeline needs no vertex stream");
        rejects([&] { sfr::encode_pipeline_manifest(one, 2); }, "invalid backend rejected");
        require(sfr::decode_pipeline_manifest(sfr::encode_pipeline_manifest({}, 0), 0).empty(), "empty manifest supported");
        const auto temporary = std::filesystem::temp_directory_path() /
            ("sfr-pipeline-manifest-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(temporary);
        const auto path = temporary / "recipes.bin";
        require(sfr::load_pipeline_manifest_file(path, 0).empty(), "missing manifest is optional");
        require(sfr::save_pipeline_manifest_file(path, one, 0), "manifest saves");
        require(sfr::pipeline_recipe_key(sfr::load_pipeline_manifest_file(path, 0).at(0)) == baseline, "file round trip");
        auto invalid_recipe = recipe;
        invalid_recipe.state.write_mask = 16;
        rejects([&] { sfr::save_pipeline_manifest_file(path, std::span(&invalid_recipe, 1), 0); },
                "failed save rejects invalid recipe");
        require(sfr::pipeline_recipe_key(sfr::load_pipeline_manifest_file(path, 0).at(0)) == baseline,
                "failed save preserves last valid file");
        require(sfr::save_pipeline_manifest_file(path, {}, 0), "existing manifest atomically replaced");
        require(sfr::load_pipeline_manifest_file(path, 0).empty(), "replacement contents loaded");
        { std::ofstream bad(path, std::ios::binary | std::ios::trunc); bad << "bad"; }
        rejects([&] { sfr::load_pipeline_manifest_file(path, 0); }, "corrupt file reported");
        require(!sfr::save_pipeline_manifest_file(path / "child", one, 0), "I/O errors reported");
        std::filesystem::remove_all(temporary);
        std::cout << "pipeline manifest regression passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
