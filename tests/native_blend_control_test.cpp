#include "native_blend_control.h"
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <utility>

using B = plume::RenderBlend;
using O = plume::RenderBlendOperation;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename F> static void rejects(F operation, const char* message) {
    try { operation(); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(message);
}

static constexpr uint32_t packed(uint32_t cs = 1, uint32_t cd = 0, uint32_t co = 0,
                                 uint32_t as = 1, uint32_t ad = 0, uint32_t ao = 0) {
    return cs | (co << 5) | (cd << 8) | (as << 16) | (ao << 21) | (ad << 24);
}

static bool same_description(const plume::RenderBlendDesc& a, const plume::RenderBlendDesc& b) {
    return a.blendEnabled == b.blendEnabled && a.srcBlend == b.srcBlend && a.dstBlend == b.dstBlend &&
        a.blendOp == b.blendOp && a.srcBlendAlpha == b.srcBlendAlpha &&
        a.dstBlendAlpha == b.dstBlendAlpha && a.blendOpAlpha == b.blendOpAlpha &&
        a.renderTargetWriteMask == b.renderTargetWriteMask;
}

static void observed_alpha_and_copy_match_plume_factories() {
    const auto alpha = sfr::decode_blend_control(0x07010706);
    require(same_description(alpha.description(15), plume::RenderBlendDesc::AlphaBlend()),
            "observed 07010706 matches all Plume AlphaBlend fields");
    const auto copy = sfr::decode_blend_control(0x00010001);
    require(same_description(copy.description(15), plume::RenderBlendDesc::Copy()) &&
            copy == sfr::NativeBlendControl{}, "exact copy tuple disables blending and matches default state");
    require(alpha == sfr::decode_blend_control(0x07010706) && !(alpha == copy),
            "native blend equality compares semantic state");
}

static void fields_and_supported_operations_are_independent() {
    const auto value = sfr::decode_blend_control(packed(4, 9, 1, 10, 5, 4));
    require(value.srcBlend == B::SRC_COLOR && value.dstBlend == B::INV_DEST_COLOR &&
            value.blendOp == O::SUBTRACT && value.srcBlendAlpha == B::DEST_ALPHA &&
            value.dstBlendAlpha == B::INV_SRC_ALPHA && value.blendOpAlpha == O::REV_SUBTRACT &&
            value.blendEnabled, "all six packed field positions decode independently");
    for (const auto& op : std::array<std::pair<uint32_t, O>, 3>{{
             {0, O::ADD}, {1, O::SUBTRACT}, {4, O::REV_SUBTRACT}}}) {
        const auto color = sfr::decode_blend_control(packed(1, 0, op.first));
        const auto alpha = sfr::decode_blend_control(packed(1, 0, 0, 1, 0, op.first));
        require(color.blendOp == op.second && color.blendOpAlpha == O::ADD &&
                alpha.blendOp == O::ADD && alpha.blendOpAlpha == op.second,
                "operations decode in either channel without relying on Plume enum numbering");
    }
}

static void factors_map_in_both_positions_and_scalarize_alpha() {
    struct Factor { uint32_t raw; B color; B alpha; };
    constexpr std::array<Factor, 12> factors{{
        {0, B::ZERO, B::ZERO}, {1, B::ONE, B::ONE},
        {4, B::SRC_COLOR, B::SRC_ALPHA}, {5, B::INV_SRC_COLOR, B::INV_SRC_ALPHA},
        {6, B::SRC_ALPHA, B::SRC_ALPHA}, {7, B::INV_SRC_ALPHA, B::INV_SRC_ALPHA},
        {8, B::DEST_COLOR, B::DEST_ALPHA}, {9, B::INV_DEST_COLOR, B::INV_DEST_ALPHA},
        {10, B::DEST_ALPHA, B::DEST_ALPHA}, {11, B::INV_DEST_ALPHA, B::INV_DEST_ALPHA},
        {12, B::BLEND_FACTOR, B::BLEND_FACTOR}, {13, B::INV_BLEND_FACTOR, B::INV_BLEND_FACTOR}
    }};
    for (const auto& factor : factors) {
        require(sfr::decode_blend_control(packed(factor.raw)).srcBlend == factor.color &&
                sfr::decode_blend_control(packed(1, factor.raw)).dstBlend == factor.color,
                "defined RGB factor maps in source and destination positions");
        require(sfr::decode_blend_control(packed(1, 0, 0, factor.raw)).srcBlendAlpha == factor.alpha &&
                sfr::decode_blend_control(packed(1, 0, 0, 1, factor.raw)).dstBlendAlpha == factor.alpha,
                "defined alpha factor scalarizes in source and destination positions");
    }
    for (uint32_t factor : {14u, 15u}) {
        const auto expected = factor == 14 ? B::BLEND_FACTOR : B::INV_BLEND_FACTOR;
        require(sfr::decode_blend_control(packed(1, 0, 0, factor)).srcBlendAlpha == expected &&
                sfr::decode_blend_control(packed(1, 0, 0, 1, factor)).dstBlendAlpha == expected,
                "alpha constant remains symbolic without inventing a constant value");
    }
    require(sfr::decode_blend_control(packed(16)).srcBlend == B::SRC_ALPHA_SAT &&
            sfr::decode_blend_control(packed(1, 16)).dstBlend == B::SRC_ALPHA_SAT,
            "RGB source-alpha saturate remains a defined RGB factor");
}

static void invalid_and_unsupported_encodings_fail_closed() {
    for (uint32_t factor = 0; factor < 32; ++factor) {
        if (factor == 2 || factor == 3 || factor >= 17) {
            for (uint32_t shift : {0u, 8u, 16u, 24u}) {
                const auto value = (packed() & ~(31u << shift)) | (factor << shift);
                rejects([&] { sfr::decode_blend_control(value); }, "reserved factor rejects in every field");
            }
        }
    }
    for (uint32_t bit : {13u, 14u, 15u, 29u, 30u, 31u})
        rejects([&] { sfr::decode_blend_control(packed() | (1u << bit)); }, "each padding bit must be zero");
    for (uint32_t op : {2u, 3u, 5u, 6u, 7u}) {
        rejects([&] { sfr::decode_blend_control(packed(1, 0, op)); }, "unsupported RGB operation rejects");
        rejects([&] { sfr::decode_blend_control(packed(1, 0, 0, 1, 0, op)); }, "unsupported alpha operation rejects");
    }
    for (uint32_t factor : {14u, 15u}) {
        rejects([&] { sfr::decode_blend_control(packed(factor)); }, "RGB scalar constant cannot use RGB blend constant");
        rejects([&] { sfr::decode_blend_control(packed(1, factor)); }, "RGB destination scalar constant rejects");
    }
    rejects([&] { sfr::decode_blend_control(packed(1, 0, 0, 16)); }, "unverified alpha saturate source rejects");
    rejects([&] { sfr::decode_blend_control(packed(1, 0, 0, 1, 16)); }, "unverified alpha saturate destination rejects");
}

static void mask_is_explicit_and_only_exact_copy_disables_blending() {
    const auto alpha = sfr::decode_blend_control(0x07010706);
    for (uint8_t mask = 0; mask < 16; ++mask) {
        auto expected = plume::RenderBlendDesc::AlphaBlend();
        expected.renderTargetWriteMask = mask;
        require(same_description(alpha.description(mask), expected), "explicit mask preserves all semantic fields");
    }
    for (unsigned mask = 16; mask < 256; ++mask)
        rejects([&] { alpha.description(static_cast<uint8_t>(mask)); }, "unknown write mask bits reject");
    for (auto word : {packed(0), packed(1, 1), packed(1, 0, 1),
                      packed(1, 0, 0, 0), packed(1, 0, 0, 1, 1), packed(1, 0, 0, 1, 0, 4)})
        require(sfr::decode_blend_control(word).blendEnabled,
                "changing any copy-tuple field keeps blending enabled");
    auto changed = alpha;
    changed.blendOpAlpha = O::SUBTRACT;
    require(!(changed == alpha), "equality includes separate alpha operation");
}

int main() {
    unsigned failures = 0;
    for (auto test : {observed_alpha_and_copy_match_plume_factories,
                      fields_and_supported_operations_are_independent,
                      factors_map_in_both_positions_and_scalarize_alpha,
                      invalid_and_unsupported_encodings_fail_closed,
                      mask_is_explicit_and_only_exact_copy_disables_blending}) {
        try { test(); }
        catch (const std::exception& error) { std::cerr << error.what() << '\n'; ++failures; }
    }
    if (failures) return 1;
    std::cout << "Native blend control checks passed\n";
    return 0;
}
