#include "guest_graphics.h"
#include "graphics_defaults_fixture.h"
#include "guest_memory.h"
#include "native_graphics.h"
#include "native_render_state.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
using State = sfr::RenderState;
using Compare = plume::RenderComparisonFunction;
constexpr uint32_t device = sfr::GuestGraphics::device_address;
constexpr uint32_t params = 0x10000000, output = params + 124;
constexpr uint32_t reciprocal = 0x82001658;
constexpr uint64_t dirty_seed = 0x8123456700000001ull;
constexpr std::array<State, 7> states{State::alpha_test_enable, State::alpha_function,
    State::alpha_reference, State::depth_enable, State::depth_function, State::depth_write, State::cull_mode};
constexpr std::array<uint32_t, 7> reached{1, 4, 0, 1, 3, 1, 6};
constexpr std::array<uint64_t, 7> dirty_masks{0x40200, 0x200, 0x08000000, 0x20800, 0x20800, 0x800, 0x40};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void rejects(F operation) {
    try { operation(); } catch (const sfr::RuntimeStop&) { return; }
    throw std::runtime_error("unsupported renderer state did not stop");
}
struct Fixture {
    sfr::GuestMemory memory;
    sfr::NativeGraphics graphics;
    sfr::GuestGraphics guest{memory, graphics};
    explicit Fixture(bool create = true, bool map_constant = true) {
        memory.map(params, 256);
        constexpr std::array<uint32_t, 31> words{
            19,11,0x18280186,1,0,0,1,0,0,1,0x1A220197,0,0,1,0,0,0x28280106,
            0,0,0,0,0,0,0,0,0,0,0,0,0,0};
        for (size_t i = 0; i < words.size(); ++i) memory.store<uint32_t>(params + i * 4, words[i]);
        memory.map(0x82AD0000, 0x2000);
        for (auto [table, count] : {std::pair{0x82AD0A60u, 101u}, std::pair{0x82AD0F20u, 20u}})
            for (uint32_t i = 0; i < count; ++i) {
                memory.store<uint32_t>(table + i * 12, 0x12340000 + i);
                memory.store<uint32_t>(table + i * 12 + 4, 0x82210000 + i * 4);
                memory.store<uint32_t>(table + i * 12 + 8, i);
            }
        sfr::test::install_blend_defaults(memory);
        sfr::test::install_sampler_defaults(memory);
        if (map_constant) {
            if (memory.available(0x82001000, 0x1000)) memory.map(0x82001000, 0x1000);
            memory.store<uint32_t>(reciprocal, 0x3B808081);
        }
        if (create) create_device();
    }
    void create_device() { require(guest.create_device(0, 1, 0, 0, params, output) == 0, "create test device"); }
    using Bytes = std::array<uint8_t, sfr::GuestGraphics::device_size>;
    Bytes bytes() const {
        Bytes result;
        std::copy_n(memory.base() + device, result.size(), result.begin());
        return result;
    }
    void preserved(const Bytes& before, const sfr::NativeRenderState& native) const {
        require(bytes() == before && guest.render_state() == native,
                "rejected setter preserves every guest device byte and every native optional field");
    }
};
void write_expected(Fixture::Bytes& bytes, uint32_t offset, uint64_t value, uint32_t size = 4) {
    for (uint32_t i = 0; i < size; ++i) bytes[offset + i] = uint8_t(value >> (8 * (size - i - 1)));
}

void reached_sequence_publishes_only_exact_original_mutations() {
    Fixture f(false);
    for (auto state : states) rejects([&] { f.guest.set_render_state(device, state, 0); });
    f.create_device();
    require(f.guest.render_state() == sfr::NativeRenderState{}, "device creation leaves every renderer state unset");
    rejects([&] { f.guest.depth_enabled(); });
    f.memory.store<uint32_t>(device + 0x293C, 0xA5A55A50);
    f.memory.store<uint32_t>(device + 0x2934, 0xC3C35A81);
    f.memory.store<uint32_t>(device + 0x2948, 0x96965A50);
    f.memory.store<uint32_t>(device + 0x3158, sfr::GuestGraphics::depth_handle);
    sfr::NativeRenderState native;
    for (size_t i = 0; i < states.size(); ++i) {
        f.memory.store<uint64_t>(device + 16, dirty_seed);
        auto expected = f.bytes();
        switch (states[i]) {
        case State::alpha_test_enable:
            write_expected(expected, 0x293C, 0xA5A55A58); native.alpha_test_enabled = true; break;
        case State::alpha_function:
            write_expected(expected, 0x293C, 0xA5A55A5C); native.alpha_function = Compare::GREATER; break;
        case State::alpha_reference:
            write_expected(expected, 0x2904, 0); native.alpha_reference = 0.0f; break;
        case State::depth_enable:
            write_expected(expected, 0x2934, 0xC3C35A83); write_expected(expected, 0x2F14, 1);
            native.depth_enable_requested = true; break;
        case State::depth_function:
            write_expected(expected, 0x2934, 0xC3C35AB3); native.depth_function = Compare::LESS_EQUAL; break;
        case State::depth_write:
            write_expected(expected, 0x2934, 0xC3C35AB7); native.depth_write_enabled = true; break;
        case State::cull_mode:
            write_expected(expected, 0x2948, 0x96965A56);
            native.cull = sfr::NativeCullState{plume::RenderCullMode::BACK, false}; break;
        }
        write_expected(expected, 16, dirty_seed | dirty_masks[i], 8);
        f.guest.set_render_state(device, states[i], reached[i]);
        require(f.bytes() == expected, "reached setter updates only its exact original cache and 64-bit dirty fields");
        require(f.guest.render_state() == native, "reached setter changes only its typed native field, preserving unset fields");
        f.memory.store<uint64_t>(device + 16, 0);
        f.guest.set_render_state(device, states[i], reached[i]);
        require(f.memory.load<uint64_t>(device + 16) == dirty_masks[i] && f.guest.render_state() == native,
                "repeated identical state still marks the exact original dirty bits");
    }
}

void comparisons_booleans_and_cull_cover_supported_domains() {
    Fixture f;
    constexpr std::array<Compare, 8> comparisons{Compare::NEVER, Compare::LESS, Compare::EQUAL,
        Compare::LESS_EQUAL, Compare::GREATER, Compare::NOT_EQUAL, Compare::GREATER_EQUAL, Compare::ALWAYS};
    for (uint32_t value = 0; value < comparisons.size(); ++value) {
        f.memory.store<uint32_t>(device + 0x293C, 0xE1E15AF8);
        f.memory.store<uint32_t>(device + 0x2934, 0xD2D25F8F);
        f.guest.set_render_state(device, State::alpha_function, value);
        f.guest.set_render_state(device, State::depth_function, value);
        require(f.guest.render_state().alpha_function == comparisons[value] &&
                f.guest.render_state().depth_function == comparisons[value] &&
                f.memory.load<uint32_t>(device + 0x293C) == (0xE1E15AF8u | value) &&
                f.memory.load<uint32_t>(device + 0x2934) == (0xD2D25F8Fu | (value << 4)),
                "all eight comparison encodings map correctly and preserve unrelated register bits");
    }
    for (uint32_t value : {0u, 1u}) {
        f.memory.store<uint32_t>(device + 0x293C, 0xA5A55A57);
        f.memory.store<uint32_t>(device + 0x2934, 0xC3C35AF9);
        f.guest.set_render_state(device, State::alpha_test_enable, value);
        f.guest.set_render_state(device, State::depth_enable, value);
        f.guest.set_render_state(device, State::depth_write, value);
        require(f.guest.render_state().alpha_test_enabled == bool(value) &&
                f.guest.render_state().depth_enable_requested == bool(value) &&
                f.guest.render_state().depth_write_enabled == bool(value) &&
                f.memory.load<uint32_t>(device + 0x293C) == (0xA5A55A57u | (value << 3)) &&
                f.memory.load<uint32_t>(device + 0x2934) == (0xC3C35AF9u | (value << 1) | (value << 2)) &&
                f.memory.load<uint32_t>(device + 0x2F14) == value,
                "canonical booleans set and clear only their original fields");
    }
    constexpr std::array<plume::RenderCullMode, 3> modes{plume::RenderCullMode::NONE,
        plume::RenderCullMode::FRONT, plume::RenderCullMode::BACK};
    for (uint32_t value : {0u, 1u, 2u, 4u, 5u, 6u}) {
        f.memory.store<uint32_t>(device + 0x2948, 0xF0F05AF8);
        f.guest.set_render_state(device, State::cull_mode, value);
        require(f.guest.render_state().cull == sfr::NativeCullState{modes[value & 3], !(value & 4)} &&
                f.memory.load<uint32_t>(device + 0x2948) == (0xF0F05AF8u | value),
                "six cull modes preserve winding and independent front/back selection");
    }
}

void alpha_reference_matches_original_float_multiply() {
    Fixture f;
    const float scale = std::bit_cast<float>(0x3B808081u);
    for (uint32_t value = 0; value <= 255; ++value) {
        const uint32_t expected = std::bit_cast<uint32_t>(float(value) * scale);
        f.memory.store<uint64_t>(device + 16, dirty_seed);
        f.guest.set_render_state(device, State::alpha_reference, value);
        require(f.guest.render_state().alpha_reference.has_value() &&
                std::bit_cast<uint32_t>(*f.guest.render_state().alpha_reference) == expected &&
                f.memory.load<uint32_t>(device + 0x2904) == expected &&
                f.memory.load<uint64_t>(device + 16) == (dirty_seed | 0x08000000),
                "all 256 alpha references use exact original single-precision multiply and dirty bits");
    }
}

void invalid_inputs_and_depth_attachment_are_transactional() {
    Fixture f;
    for (size_t i = 0; i < states.size(); ++i) f.guest.set_render_state(device, states[i], reached[i]);
    const auto native = f.guest.render_state();
    const auto bytes = f.bytes();
    for (auto state : states) {
        rejects([&] { f.guest.set_render_state(device + 4, state, 0); }); f.preserved(bytes, native);
        rejects([&] { f.guest.set_render_state(0, state, 0); }); f.preserved(bytes, native);
        for (uint32_t value : {256u, UINT32_MAX}) {
            rejects([&] { f.guest.set_render_state(device, state, value); }); f.preserved(bytes, native);
        }
    }
    for (auto state : {State::alpha_test_enable, State::depth_enable, State::depth_write}) {
        rejects([&] { f.guest.set_render_state(device, state, 2); }); f.preserved(bytes, native);
    }
    for (auto state : {State::alpha_function, State::depth_function}) {
        rejects([&] { f.guest.set_render_state(device, state, 8); }); f.preserved(bytes, native);
    }
    for (uint32_t value : {3u, 7u, 8u}) {
        rejects([&] { f.guest.set_render_state(device, State::cull_mode, value); }); f.preserved(bytes, native);
    }
    for (uint32_t value : {0u, 0x61u, UINT32_MAX}) {
        rejects([&] { f.guest.set_render_state(device, static_cast<State>(value), 0); }); f.preserved(bytes, native);
    }
    require(f.guest.depth_enabled(), "known attached native depth target makes requested depth effective");
    f.memory.store<uint32_t>(device + 0x3158, 0);
    require(!f.guest.depth_enabled() && f.guest.render_state().depth_enable_requested == true,
            "effective depth dynamically follows detachment without losing the request");
    f.guest.set_render_state(device, State::depth_enable, 1);
    require(!(f.memory.load<uint32_t>(device + 0x2934) & 2), "setter mirrors disabled depth while attachment absent");
    f.memory.store<uint32_t>(device + 0x3158, sfr::GuestGraphics::depth_handle);
    require(f.guest.depth_enabled(), "effective depth recomputes on known reattachment");
    f.guest.set_render_state(device, State::depth_enable, 0);
    require(!f.guest.depth_enabled(), "requested false disables depth despite known attachment");
    f.memory.store<uint32_t>(device + 0x3158, 0xBAD00000);
    const auto unknown_bytes = f.bytes(); const auto unknown_native = f.guest.render_state();
    rejects([&] { f.guest.depth_enabled(); });
    for (uint32_t value : {0u, 1u}) {
        rejects([&] { f.guest.set_render_state(device, State::depth_enable, value); });
        f.preserved(unknown_bytes, unknown_native);
    }
}

void guards_and_invalid_constant_publish_nothing() {
    for (size_t i = 0; i < states.size(); ++i) {
        Fixture f;
        const auto initial_native = f.guest.render_state();
        const auto initial_bytes = f.bytes();
        const auto word = f.memory.load_reserved_word(params);
        rejects([&] { f.guest.set_render_state(device, states[i], reached[i]); });
        f.preserved(initial_bytes, initial_native);
        require(f.memory.has_reservation() && f.memory.store_conditional_word(params, word),
                "failed setter preserves live reservation and conditional-store success");
        unsigned samples = 0;
        f.memory.add_read_only_word(device + 20, [&] { ++samples; return 0u; });
        rejects([&] { f.guest.set_render_state(device, states[i], reached[i]); });
        f.preserved(initial_bytes, initial_native);
        require(samples == 0, "late dirty-word provider guard is preflighted before reading or publishing");
    }
    Fixture imported;
    const auto imported_native = imported.guest.render_state(); const auto imported_bytes = imported.bytes();
    imported.memory.add_import_variable(device + 20, "RenderStateDirtyGuard");
    for (size_t i = 0; i < states.size(); ++i) {
        rejects([&] { imported.guest.set_render_state(device, states[i], reached[i]); });
        imported.preserved(imported_bytes, imported_native);
    }
    Fixture f;
    f.guest.set_render_state(device, State::alpha_reference, 128);
    const auto native = f.guest.render_state(); const auto bytes = f.bytes();
    for (uint32_t constant : {0u, 0x3B808080u, 0x3F800000u, 0x7FC00000u}) {
        f.memory.store<uint32_t>(reciprocal, constant);
        rejects([&] { f.guest.set_render_state(device, State::alpha_reference, 255); });
        f.preserved(bytes, native);
    }
    Fixture unmapped(true, false);
    const auto empty_native = unmapped.guest.render_state(); const auto empty_bytes = unmapped.bytes();
    rejects([&] { unmapped.guest.set_render_state(device, State::alpha_reference, 0); });
    unmapped.preserved(empty_bytes, empty_native);
}

void primitive_restart_preserves_full_device_and_uses_only_low_bit() {
    Fixture f(false);
    rejects([&] { f.guest.set_primitive_restart(device, 1); });
    require(!f.guest.created(), "restart before device creation must not create a device");
    f.create_device();
    require(!f.guest.render_state().primitive_restart_enabled.has_value(),
            "device creation leaves primitive restart unknown, not implicitly disabled");
    f.memory.store<uint32_t>(device + 0x2948, 0x96965A56);
    f.memory.store<uint64_t>(device + 16, dirty_seed);
    auto expected = f.bytes();
    auto native = f.guest.render_state();
    write_expected(expected, 0x2948, 0x96B65A56);
    write_expected(expected, 16, dirty_seed | 0x40, 8);
    native.primitive_restart_enabled = true;
    f.guest.set_primitive_restart(device, 1);
    require(f.bytes() == expected && f.guest.render_state() == native,
            "first restart changes only bit21 and dirty40, preserving all other unknown state");

    for (size_t i = 0; i < states.size(); ++i)
        f.guest.set_render_state(device, states[i], reached[i]);
    require(f.guest.render_state().primitive_restart_enabled == true &&
            (f.memory.load<uint32_t>(device + 0x2948) & 0x00200000),
            "existing state setters, including shared-word cull, preserve restart");
    native = f.guest.render_state();
    constexpr std::array<uint32_t, 12> values{0, 1, 2, 3, 0x80000000, 0x80000001,
        0xFFFFFFFE, UINT32_MAX, 0x00200000, 0x00200001, 0x40000000, 0x40000001};
    for (uint32_t value : values) {
        // Deliberately start from both bit21 states and nonzero neighboring bits.
        for (uint32_t old : {0x96965A56u, 0x96B65A56u}) {
            f.memory.store<uint32_t>(device + 0x2948, old);
            f.memory.store<uint64_t>(device + 16, dirty_seed);
            expected = f.bytes();
            write_expected(expected, 0x2948, (value & 1) ? 0x96B65A56 : 0x96965A56);
            write_expected(expected, 16, dirty_seed | 0x40, 8);
            native.primitive_restart_enabled = bool(value & 1);
            f.guest.set_primitive_restart(device, value);
            require(f.bytes() == expected && f.guest.render_state() == native,
                    "every uint32 input uses only its low bit and preserves all other device/native state");
            f.memory.store<uint64_t>(device + 16, 0);
            expected = f.bytes();
            write_expected(expected, 16, 0x40, 8);
            f.guest.set_primitive_restart(device, value);
            require(f.bytes() == expected && f.guest.render_state() == native,
                    "repeated identical restart still performs the exact original dirty write");
        }
    }
}

void primitive_restart_guards_publish_no_guest_or_native_effects() {
    Fixture f;
    f.guest.set_render_state(device, State::cull_mode, 6);
    f.guest.set_primitive_restart(device, 1);
    auto before = f.bytes();
    const auto native = f.guest.render_state();
    for (uint32_t owner : {0u, device + 4, sfr::GuestGraphics::color_handle,
                           sfr::GuestGraphics::depth_handle, UINT32_MAX}) {
        rejects([&] { f.guest.set_primitive_restart(owner, 0); });
        f.preserved(before, native);
    }
    const auto reserved = f.memory.load_reserved_word(params);
    rejects([&] { f.guest.set_primitive_restart(device, 0); });
    f.preserved(before, native);
    require(f.memory.has_reservation() && f.memory.store_conditional_word(params, reserved),
            "restart rejection preserves a live atomic reservation and subsequent store-conditional");
    unsigned samples = 0;
    f.memory.add_read_only_word(device + 20, [&] { ++samples; return 0u; });
    for (uint32_t value : {0u, UINT32_MAX}) {
        rejects([&] { f.guest.set_primitive_restart(device, value); });
        f.preserved(before, native);
    }
    require(samples == 0, "late dirty provider is rejected by preflight before reads or publication");

    Fixture cache_guard;
    cache_guard.guest.set_primitive_restart(device, 1);
    const auto cache_bytes = cache_guard.bytes();
    const auto cache_native = cache_guard.guest.render_state();
    unsigned cache_samples = 0;
    cache_guard.memory.add_read_only_word(device + 0x2948, [&] { ++cache_samples; return 0u; });
    rejects([&] { cache_guard.guest.set_primitive_restart(device, 0); });
    cache_guard.preserved(cache_bytes, cache_native);
    require(cache_samples == 0, "cache provider is rejected before the cache is read");

    Fixture imported;
    imported.guest.set_primitive_restart(device, 1);
    const auto imported_bytes = imported.bytes();
    const auto imported_native = imported.guest.render_state();
    imported.memory.add_import_variable(device + 20, "PrimitiveRestartDirtyGuard");
    rejects([&] { imported.guest.set_primitive_restart(device, 0); });
    imported.preserved(imported_bytes, imported_native);
}
}

int main() {
    unsigned failures = 0;
    for (auto test : {reached_sequence_publishes_only_exact_original_mutations,
                      comparisons_booleans_and_cull_cover_supported_domains,
                      alpha_reference_matches_original_float_multiply,
                      invalid_inputs_and_depth_attachment_are_transactional,
                      guards_and_invalid_constant_publish_nothing,
                      primitive_restart_preserves_full_device_and_uses_only_low_bit,
                      primitive_restart_guards_publish_no_guest_or_native_effects}) {
        try { test(); } catch (const std::exception& error) { std::cerr << error.what() << '\n'; ++failures; }
    }
    if (failures) return 1;
    std::cout << "Guest renderer state tests passed\n";
    return 0;
}
