#include "pipeline_manifest.h"
#include "shader_pack_format.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>

namespace sfr {
namespace {
constexpr std::string_view magic = "SFRPMNF1";
constexpr uint32_t format_version = 1;
constexpr size_t header_bytes = 32, checksum_bytes = 8;
constexpr size_t max_record_bytes = 2048;
constexpr uint32_t max_stride = 2048;
constexpr std::array<const char*, 14> semantics{
    "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT",
    "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE"};

[[noreturn]] void invalid(const char* reason) { throw std::invalid_argument(reason); }
void check(bool condition, const char* reason) { if (!condition) invalid(reason); }
uint64_t hash_bytes(std::span<const uint8_t> bytes) {
    uint64_t hash = 14695981039346656037ull;
    for (auto byte : bytes) hash = (hash ^ byte) * 1099511628211ull;
    return hash;
}
void put(std::vector<uint8_t>& bytes, uint64_t value, size_t count = 4) {
    for (size_t i = 0; i < count; ++i) bytes.push_back(uint8_t(value >> (8 * i)));
}
struct Reader {
    std::span<const uint8_t> bytes;
    size_t at = 0;
    std::span<const uint8_t> take(size_t count) {
        check(count <= bytes.size() - at, "truncated pipeline manifest");
        const auto result = bytes.subspan(at, count);
        at += count;
        return result;
    }
    uint64_t number(size_t count = 4) {
        const auto data = take(count);
        uint64_t value = 0;
        for (size_t i = 0; i < count; ++i) value |= uint64_t(data[i]) << (8 * i);
        return value;
    }
    uint32_t word() { return uint32_t(number()); }
    bool boolean() {
        const auto value = word();
        check(value <= 1, "invalid pipeline boolean");
        return value != 0;
    }
    uint8_t byte() {
        const auto value = word();
        check(value <= 255, "invalid pipeline byte field");
        return uint8_t(value);
    }
    template<class E> E enumeration() { return static_cast<E>(word()); }
    bool end() const { return at == bytes.size(); }
};

template<class E> void enum_range(E value, E first, E last) {
    check(uint32_t(value) >= uint32_t(first) && uint32_t(value) <= uint32_t(last),
          "unsupported pipeline enumeration");
}
uint32_t semantic_id(const char* name) {
    check(name != nullptr, "missing pipeline semantic name");
    for (uint32_t i = 0; i < semantics.size(); ++i)
        if (std::strcmp(name, semantics[i]) == 0) return i;
    invalid("unsupported pipeline semantic name");
}
uint32_t format_bytes(plume::RenderFormat format) {
    // The guest declaration decoder emits this explicit portable vertex set.
    // In-range texture, depth and typeless formats must never reach a driver.
    using F = plume::RenderFormat;
    switch (format) {
    case F::R32G32B32A32_FLOAT: return 16;
    case F::R32G32B32_FLOAT: return 12;
    case F::R32G32_FLOAT:
    case F::R16G16B16A16_FLOAT: case F::R16G16B16A16_SINT:
    case F::R16G16B16A16_SNORM: case F::R16G16B16A16_UNORM: return 8;
    case F::R32_FLOAT: case F::R32_UINT:
    case F::B8G8R8A8_UNORM: case F::R8G8B8A8_UINT: case F::R8G8B8A8_UNORM:
    case F::R16G16_FLOAT: case F::R16G16_SINT:
    case F::R16G16_SNORM: case F::R16G16_UNORM: return 4;
    default: invalid("unsupported pipeline vertex format");
    }
}
void validate(const PipelineRecipe& recipe) {
    using namespace plume;
    check(recipe.vertex.size && recipe.pixel.size, "missing pipeline shader identity");
    const auto& d = recipe.state;
    enum_range(d.topology, RenderPrimitiveTopology::POINT_LIST, RenderPrimitiveTopology::TRIANGLE_STRIP);
    enum_range(d.cull, RenderCullMode::NONE, RenderCullMode::BACK);
    enum_range(d.depth_function, RenderComparisonFunction::NEVER, RenderComparisonFunction::ALWAYS);
    for (auto factor : {d.blend.srcBlend, d.blend.dstBlend, d.blend.srcBlendAlpha, d.blend.dstBlendAlpha})
        enum_range(factor, RenderBlend::ZERO, RenderBlend::INV_BLEND_FACTOR);
    for (auto operation : {d.blend.blendOp, d.blend.blendOpAlpha})
        enum_range(operation, RenderBlendOperation::ADD, RenderBlendOperation::MAX);
    // D3D12 does not allow color factors or SRC_ALPHA_SAT for alpha blending.
    for (auto factor : {d.blend.srcBlendAlpha, d.blend.dstBlendAlpha})
        check(factor != RenderBlend::SRC_COLOR && factor != RenderBlend::INV_SRC_COLOR &&
              factor != RenderBlend::DEST_COLOR && factor != RenderBlend::INV_DEST_COLOR &&
              factor != RenderBlend::SRC_ALPHA_SAT, "unsupported pipeline alpha blend factor");
    check(d.write_mask <= 15, "invalid pipeline color mask");
    check(d.stride <= max_stride, "pipeline stride exceeds bound");
    check(d.elements.size() <= pipeline_manifest_max_elements, "too many pipeline input elements");
    uint32_t locations = 0;
    std::set<std::pair<uint32_t, uint32_t>> names;
    for (const auto& e : d.elements) {
        const auto semantic = semantic_id(e.semanticName);
        check(e.semanticIndex <= 31, "pipeline semantic index exceeds bound");
        check(e.location < 32 && !(locations & (uint32_t(1) << e.location)), "invalid or duplicate pipeline input location");
        locations |= uint32_t(1) << e.location;
        check(names.insert({semantic, e.semanticIndex}).second, "duplicate pipeline input semantic");
        const auto size = format_bytes(e.format);
        check(e.slotIndex == 0 || e.slotIndex == 15, "unsupported pipeline input slot");
        const uint32_t limit = e.slotIndex == 0 ? d.stride : 256;
        check(size <= limit && e.alignedByteOffset <= limit - size, "pipeline input exceeds vertex storage");
    }
    if (d.stencil_enabled) {
        for (auto face : {d.stencil_front, d.stencil_back}) {
            for (auto op : {face.passOp, face.failOp, face.depthFailOp})
                enum_range(op, RenderStencilOp::KEEP, RenderStencilOp::DECREMENT_AND_WRAP);
            enum_range(face.compareFunction, RenderComparisonFunction::NEVER, RenderComparisonFunction::ALWAYS);
        }
    }
}
void validate_backend(uint32_t backend) { check(backend <= 1, "unsupported pipeline manifest backend"); }
PipelineRecipe read_recipe(std::span<const uint8_t> bytes) {
    Reader in{bytes};
    PipelineRecipe recipe;
    recipe.vertex = {in.number(8), in.word()};
    recipe.pixel = {in.number(8), in.word()};
    recipe.pixel_link_constants = in.word();
    auto& d = recipe.state;
    d.pixel_spec_constants = in.word();
    d.stride = in.word();
    d.topology = in.enumeration<plume::RenderPrimitiveTopology>();
    const auto count = in.word();
    check(count <= pipeline_manifest_max_elements, "too many pipeline input elements");
    d.elements.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const auto semantic = in.word();
        check(semantic < semantics.size(), "unsupported pipeline semantic name");
        plume::RenderInputElement e;
        e.semanticName = semantics[semantic];
        e.semanticIndex = in.word();
        e.location = in.word();
        e.format = in.enumeration<plume::RenderFormat>();
        e.slotIndex = in.word();
        e.alignedByteOffset = in.word();
        d.elements.push_back(e);
    }
    d.blend.srcBlend = in.enumeration<plume::RenderBlend>();
    d.blend.dstBlend = in.enumeration<plume::RenderBlend>();
    d.blend.blendOp = in.enumeration<plume::RenderBlendOperation>();
    d.blend.srcBlendAlpha = in.enumeration<plume::RenderBlend>();
    d.blend.dstBlendAlpha = in.enumeration<plume::RenderBlend>();
    d.blend.blendOpAlpha = in.enumeration<plume::RenderBlendOperation>();
    d.blend.blendEnabled = in.boolean();
    d.write_mask = in.byte();
    d.depth_enabled = in.boolean();
    d.depth_write = in.boolean();
    d.depth_function = in.enumeration<plume::RenderComparisonFunction>();
    d.cull = in.enumeration<plume::RenderCullMode>();
    d.stencil_enabled = in.boolean();
    if (d.stencil_enabled) {
        d.stencil_reference = in.byte();
        d.stencil_read_mask = in.byte();
        d.stencil_write_mask = in.byte();
        for (auto* face : {&d.stencil_front, &d.stencil_back}) {
            face->passOp = in.enumeration<plume::RenderStencilOp>();
            face->failOp = in.enumeration<plume::RenderStencilOp>();
            face->depthFailOp = in.enumeration<plume::RenderStencilOp>();
            face->compareFunction = in.enumeration<plume::RenderComparisonFunction>();
        }
    }
    check(in.end(), "trailing pipeline recipe fields");
    validate(recipe);
    return recipe;
}
}

PipelineShaderId pipeline_shader_id(std::span<const uint8_t> bytes) {
    check(bytes.size() <= (std::numeric_limits<uint32_t>::max)(), "shader source exceeds identity size bound");
    return {hash_bytes(bytes), uint32_t(bytes.size())};
}
std::vector<uint8_t> pipeline_recipe_key(const PipelineRecipe& recipe) {
    validate(recipe);
    std::vector<uint8_t> bytes;
    bytes.reserve(140 + 24 * recipe.state.elements.size());
    const auto word = [&](auto value) { put(bytes, uint32_t(value)); };
    put(bytes, recipe.vertex.hash, 8); word(recipe.vertex.size);
    put(bytes, recipe.pixel.hash, 8); word(recipe.pixel.size);
    word(recipe.pixel_link_constants);
    const auto& d = recipe.state;
    word(d.pixel_spec_constants); word(d.stride); word(d.topology); word(d.elements.size());
    for (const auto& e : d.elements) {
        word(semantic_id(e.semanticName)); word(e.semanticIndex); word(e.location);
        word(e.format); word(e.slotIndex); word(e.alignedByteOffset);
    }
    word(d.blend.srcBlend); word(d.blend.dstBlend); word(d.blend.blendOp);
    word(d.blend.srcBlendAlpha); word(d.blend.dstBlendAlpha); word(d.blend.blendOpAlpha); word(d.blend.blendEnabled);
    word(d.write_mask); word(d.depth_enabled); word(d.depth_write); word(d.depth_function); word(d.cull);
    word(d.stencil_enabled);
    if (d.stencil_enabled) {
        word(d.stencil_reference); word(d.stencil_read_mask); word(d.stencil_write_mask);
        for (auto face : {d.stencil_front, d.stencil_back}) {
            word(face.passOp); word(face.failOp); word(face.depthFailOp); word(face.compareFunction);
        }
    }
    return bytes;
}
std::vector<uint8_t> encode_pipeline_manifest(std::span<const PipelineRecipe> recipes, uint32_t backend) {
    validate_backend(backend);
    check(recipes.size() <= pipeline_manifest_max_recipes, "too many pipeline recipes");
    std::vector<uint8_t> payload;
    std::set<std::vector<uint8_t>> seen;
    for (const auto& recipe : recipes) {
        auto key = pipeline_recipe_key(recipe);
        if (!seen.insert(key).second) continue;
        put(payload, key.size());
        payload.insert(payload.end(), key.begin(), key.end());
    }
    std::vector<uint8_t> bytes(magic.begin(), magic.end());
    put(bytes, format_version); put(bytes, shader_abi_version); put(bytes, backend); put(bytes, seen.size());
    put(bytes, payload.size(), 8);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    put(bytes, hash_bytes(bytes), checksum_bytes);
    check(bytes.size() <= pipeline_manifest_max_bytes, "pipeline manifest exceeds file bound");
    return bytes;
}
std::vector<PipelineRecipe> decode_pipeline_manifest(std::span<const uint8_t> bytes, uint32_t backend) {
    validate_backend(backend);
    check(bytes.size() >= header_bytes + checksum_bytes && bytes.size() <= pipeline_manifest_max_bytes,
          "pipeline manifest size outside bounds");
    Reader in{bytes.first(bytes.size() - checksum_bytes)};
    const auto signature = in.take(magic.size());
    check(std::equal(signature.begin(), signature.end(), magic.begin()), "invalid pipeline manifest magic");
    check(in.word() == format_version, "pipeline manifest format version mismatch");
    check(in.word() == shader_abi_version, "pipeline manifest shader ABI mismatch");
    check(in.word() == backend, "pipeline manifest backend mismatch");
    const auto count = in.word();
    check(count <= pipeline_manifest_max_recipes, "too many pipeline recipes");
    check(in.number(8) == bytes.size() - header_bytes - checksum_bytes, "pipeline manifest payload size mismatch");
    Reader tail{bytes.last(checksum_bytes)};
    check(tail.number(8) == hash_bytes(in.bytes), "pipeline manifest checksum mismatch");
    // Even valid checksums may describe corrupt or hand-edited state. Validate
    // each complete record before allowing any recipe to escape this function.
    std::vector<PipelineRecipe> recipes;
    std::set<std::vector<uint8_t>> seen;
    for (uint32_t i = 0; i < count; ++i) {
        const auto length = in.word();
        check(length <= max_record_bytes, "pipeline recipe exceeds record bound");
        const auto record = in.take(length);
        auto recipe = read_recipe(record);
        if (seen.insert(pipeline_recipe_key(recipe)).second) recipes.push_back(std::move(recipe));
    }
    check(in.end(), "trailing pipeline manifest records");
    return recipes;
}
std::vector<PipelineRecipe> load_pipeline_manifest_file(const std::filesystem::path& path, uint32_t backend) {
    validate_backend(backend);
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        if (!exists && !error) return {};
        invalid("could not open pipeline manifest");
    }
    const auto length = input.tellg();
    check(length >= std::streamoff(header_bytes + checksum_bytes) && length <= std::streamoff(pipeline_manifest_max_bytes),
          "pipeline manifest file size outside bounds");
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    input.seekg(0);
    check(bool(input.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()))), "could not read pipeline manifest");
    return decode_pipeline_manifest(bytes, backend);
}
bool save_pipeline_manifest_file(const std::filesystem::path& path, std::span<const PipelineRecipe> recipes, uint32_t backend) {
    const auto bytes = encode_pipeline_manifest(recipes, backend);
    std::error_code error;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    static std::atomic<uint64_t> sequence{0};
    auto temporary = path;
    temporary += ".new-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "-" + std::to_string(sequence.fetch_add(1));
    const auto failed = [&] {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    };
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return failed();
        output.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        output.close();
        if (!output) return failed();
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return failed();
#else
    std::filesystem::rename(temporary, path, error);
    if (error) return failed();
#endif
    return true;
}
}
