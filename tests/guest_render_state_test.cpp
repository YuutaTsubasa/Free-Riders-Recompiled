#include "guest_graphics.h"
#include "graphics_defaults_fixture.h"
#include "guest_memory.h"
#include "native_graphics.h"
#include "native_render_state.h"
#include "original_stencil_setter_fixture.h"
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

bool same_face(const plume::RenderStencilFaceDesc& a, const plume::RenderStencilFaceDesc& b) {
    return a.passOp==b.passOp && a.failOp==b.failOp && a.depthFailOp==b.depthFailOp &&
           a.compareFunction==b.compareFunction;
}
bool same_stencil(const sfr::NativeStencilState& a, const sfr::NativeStencilState& b) {
    return a.control==b.control && a.refmask==b.refmask && a.enabled==b.enabled &&
           a.reference==b.reference && a.read_mask==b.read_mask && a.write_mask==b.write_mask &&
           same_face(a.front,b.front) && same_face(a.back,b.back);
}
void stencil_original_setters_decode_every_face_field() {
    Fixture f;
    using Op=plume::RenderStencilOp;
    constexpr std::array comparisons{Compare::NEVER,Compare::LESS,Compare::EQUAL,Compare::LESS_EQUAL,
        Compare::GREATER,Compare::NOT_EQUAL,Compare::GREATER_EQUAL,Compare::ALWAYS};
    constexpr std::array operations{Op::KEEP,Op::ZERO,Op::REPLACE,Op::INCREMENT_AND_CLAMP,
        Op::DECREMENT_AND_CLAMP,Op::INVERT,Op::INCREMENT_AND_WRAP,Op::DECREMENT_AND_WRAP};
    struct Setter { uint32_t entry,mask,shift; uint64_t dirty; bool back; unsigned field; };
    constexpr std::array setters{
        Setter{0x824E71F0,0x00000700,8,0x800,false,0},
        Setter{0x824E7220,0x00003800,11,0x20800,false,1},
        Setter{0x824E7258,0x000E0000,17,0x20800,false,2},
        Setter{0x824E7290,0x0001C000,14,0x800,false,3},
        Setter{0x824E72C0,0x00700000,20,0x800,true,0},
        Setter{0x824E72F0,0x03800000,23,0x20800,true,1},
        Setter{0x824E7328,0xE0000000,29,0x20800,true,2},
        Setter{0x824E7360,0x1C000000,26,0x800,true,3}};
    constexpr uint32_t seed=0x96374D81;
    for (const auto& setter : setters) for (uint32_t value=0; value<8; ++value) {
        f.memory.store<uint32_t>(device+10548,seed);
        f.memory.store<uint32_t>(device+10496,0xCAFEDCBA);
        f.guest.invalidate_stencil_state();
        require(f.guest.prepare_stencil_state(true)==8,"initial stencil reads exactly two words");
        auto expected=f.guest.stencil_state();
        auto& face=setter.back ? expected.back : expected.front;
        switch (setter.field) {
        case 0: face.compareFunction=comparisons[value]; break;
        case 1: face.failOp=operations[value]; break;
        case 2: face.depthFailOp=operations[value]; break;
        case 3: face.passOp=operations[value]; break;
        }
        expected.control=(seed&~setter.mask)|(value<<setter.shift);
        f.memory.store<uint64_t>(device+16,dirty_seed);
        // Upper bits are intentionally nonzero: the original rotate/insert
        // instructions select only the three encoded comparison/operation bits.
        sfr::test::original_stencil_setter(f.memory,setter.entry,device,value|0xF0000008u);
        require(f.memory.load<uint32_t>(device+10548)==expected.control &&
                f.memory.load<uint64_t>(device+16)==(dirty_seed|setter.dirty),
                "fixed original stencil opcode changes only its field and original dirty bits");
        f.guest.invalidate_stencil_state(sfr::GuestGraphics::stencil_control_pending);
        const auto before=f.bytes();
        require(f.guest.prepare_stencil_state(true)==4 && same_stencil(f.guest.stencil_state(),expected),
                "all front/back comparison and operation encodings match original setters independently");
        require(f.bytes()==before,"stencil preparation leaves every guest device byte intact");
    }
    for (uint32_t value : {0u,1u,2u,3u,UINT32_MAX}) {
        sfr::test::original_stencil_setter(f.memory,0x824E71B8,device,value);
        f.guest.invalidate_stencil_state(sfr::GuestGraphics::stencil_control_pending);
        f.guest.prepare_stencil_state(true);
        require(bool(f.guest.stencil_state().control&0x80)==bool(value&1),
                "original back-face enable consumes only the low bit");
        if (!(value&1)) require(same_face(f.guest.stencil_state().front,f.guest.stencil_state().back),
                               "disabled two-sided stencil mirrors the entire front descriptor");
        for (uint32_t attachment : {0u,sfr::GuestGraphics::depth_handle}) {
            f.memory.store<uint32_t>(device+12632,attachment);
            sfr::test::original_stencil_setter(f.memory,0x824E7178,device,value);
            f.guest.invalidate_stencil_state(sfr::GuestGraphics::stencil_control_pending);
            f.guest.prepare_stencil_state(true);
            require(f.guest.stencil_state().enabled==bool(attachment && (value&1)) &&
                    f.memory.load<uint32_t>(device+12056)==value,
                    "original enable keeps the full request but gates its low bit by attachment");
        }
    }
}

void stencil_original_byte_setters_preserve_neighbors() {
    Fixture f;
    f.guest.prepare_stencil_state();
    constexpr std::array<uint32_t,3> entries{0x824E7390,0x824E73B0,0x824E73D0};
    for (uint32_t field=0; field<entries.size(); ++field) for (uint32_t value=0; value<256; ++value) {
        f.memory.store<uint32_t>(device+10496,0xD4C3B2A1);
        f.guest.invalidate_stencil_state(sfr::GuestGraphics::stencil_refmask_pending);
        f.guest.prepare_stencil_state();
        auto expected=f.guest.stencil_state();
        expected.refmask=(0xD4C3B2A1u&~(0xffu<<(field*8)))|(value<<(field*8));
        if (field==0) expected.reference=uint8_t(value);
        if (field==1) expected.read_mask=uint8_t(value);
        if (field==2) expected.write_mask=uint8_t(value);
        f.memory.store<uint64_t>(device+16,dirty_seed);
        sfr::test::original_stencil_setter(f.memory,entries[field],device,0xA5A50000|value);
        require(f.memory.load<uint32_t>(device+10496)==expected.refmask &&
                f.memory.load<uint64_t>(device+16)==(dirty_seed|0x10000000),
                "original byte store truncates inputs and preserves neighboring and reserved bytes");
        f.guest.invalidate_stencil_state(sfr::GuestGraphics::stencil_refmask_pending);
        require(f.guest.prepare_stencil_state(true)==4 && same_stencil(f.guest.stencil_state(),expected),
                "every reference/read-mask/write-mask byte decodes independently");
    }
}

void stencil_pending_packets_and_shared_depth_word() {
    Fixture f(false);
    f.guest.invalidate_stencil_state();
    f.guest.invalidate_stencil_packet(device+10548,UINT64_MAX);
    rejects([&] { f.guest.prepare_stencil_state(); });
    rejects([&] { (void)f.guest.stencil_state(); });
    f.create_device();
    require(f.guest.prepare_stencil_state()==8 && f.guest.prepare_stencil_state()==0,
            "fresh device starts with both words pending and consumes them once");
    // The packet sender may clear guest dirty flags before native draw consumes
    // the packet. Host pending state must survive that original protocol.
    f.memory.store<uint32_t>(device+10548,0xE7D6C581);
    f.memory.store<uint32_t>(device+10496,0xAC543210);
    f.guest.invalidate_stencil_packet(device+10444,(uint64_t(1)<<50)|(uint64_t(1)<<37));
    f.memory.store<uint64_t>(device+16,0);
    require(f.guest.prepare_stencil_state(true)==8 &&
            f.guest.stencil_state().control==0xE7D6C581 && f.guest.stencil_state().refmask==0xAC543210 &&
            f.memory.load<uint64_t>(device+16)==0,
            "nonadjacent MSB-first packet selections refresh both words after dirty is cleared");
    struct Packet { uint32_t source; uint64_t selected; uint32_t bytes; };
    constexpr std::array packets{
        Packet{device+10548,1ull<<63,4}, Packet{device+10496,1ull<<63,4},
        Packet{device+10548-252,1,4}, Packet{device+10496-252,1,4},
        Packet{device+10548-256,1,0}, Packet{device+10496-256,1,0},
        Packet{device+10548-1,UINT64_MAX,0}, Packet{device+10496-1,UINT64_MAX,0},
        Packet{device+10548+4,UINT64_MAX,0}, Packet{UINT32_MAX-3,UINT64_MAX,0},
        Packet{device+10444,1ull<<49,0}, Packet{device+10548,0,0}};
    for (const auto& packet : packets) {
        f.guest.invalidate_stencil_packet(packet.source,packet.selected);
        require(f.guest.prepare_stencil_state()==packet.bytes,
                "packet coverage respects selected bit, word63, word64, alignment and address bounds");
    }
    f.memory.store<uint32_t>(device+12632,sfr::GuestGraphics::depth_handle);
    for (const auto [state,value] : {std::pair{State::depth_enable,1u},
                                   std::pair{State::depth_write,1u},
                                   std::pair{State::depth_function,6u}}) {
        const auto old=f.guest.stencil_state();
        f.guest.set_render_state(device,state,value);
        require(f.guest.prepare_stencil_state(true)==4 &&
                f.guest.stencil_state().control==f.memory.load<uint32_t>(device+10548) &&
                same_face(f.guest.stencil_state().front,old.front) &&
                same_face(f.guest.stencil_state().back,old.back) &&
                f.guest.stencil_state().enabled==old.enabled,
                "native depth setters notify the shared word without changing stencil faces");
    }
    f.guest.set_render_state(device,State::cull_mode,6);
    require(f.guest.prepare_stencil_state()==0,"unrelated native state leaves stencil clean");
    // Embedded shader / bulk replacement / reset hooks all use this same
    // conservative notification API. Hook routing is separately source-audited;
    // this fixture does not claim to execute those original bulk entrypoints.
    for (uint32_t pattern : {0xFFFFFFFFu,0u,0x13579BDFu}) {
        f.memory.store<uint32_t>(device+10548,pattern);
        f.memory.store<uint32_t>(device+10496,~pattern);
        f.guest.invalidate_stencil_state(); f.guest.invalidate_stencil_state();
        f.memory.store<uint64_t>(device+16,0);
        require(f.guest.prepare_stencil_state(true)==8 && f.guest.prepare_stencil_state()==0 &&
                f.guest.stencil_state().control==pattern && f.guest.stencil_state().refmask==~pattern,
                "bulk/reset/embedded notifications coalesce and preserve new source words");
    }
    Fixture fresh;
    require(fresh.guest.prepare_stencil_state()==8 &&
            fresh.guest.stencil_state().control==fresh.memory.load<uint32_t>(device+10548) &&
            fresh.guest.stencil_state().refmask==fresh.memory.load<uint32_t>(device+10496),
            "a new owner at the same guest device address does not inherit the prior snapshot");
}

void stencil_clean_reads_and_verifier_are_independent() {
    Fixture f;
    uint32_t control=0xA9876581, refmask=0xDE123456;
    unsigned control_reads=0, refmask_reads=0;
    f.memory.add_read_only_word(device+10548,[&] { ++control_reads; return control; });
    f.memory.add_read_only_word(device+10496,[&] { ++refmask_reads; return refmask; });
    require(f.guest.prepare_stencil_state()==8 && control_reads==1 && refmask_reads==1,
            "first stencil preparation reads each source word exactly once");
    const auto before=f.guest.stencil_state();
    control^=1; refmask^=0xff;
    require(f.guest.prepare_stencil_state()==0 && control_reads==1 && refmask_reads==1 &&
            same_stencil(f.guest.stencil_state(),before),
            "clean preparation reads neither source nor unnotified changes");
    const auto mismatch=[&](uint32_t address) {
        try { f.guest.prepare_stencil_state(true); }
        catch (const sfr::RuntimeStop& error) {
            require(error.category=="managed-stencil-state-mismatch" && error.address==address,
                    "verifier reports the exact unnotified raw word");
            require(same_stencil(f.guest.stencil_state(),before),
                    "failed verification never partially publishes pending fields");
            return;
        }
        throw std::runtime_error("stencil verifier adopted an unnotified source change");
    };
    mismatch(device+10548);
    require(control_reads==2 && refmask_reads==2,"clean verifier independently reads both words");
    f.guest.invalidate_stencil_state(sfr::GuestGraphics::stencil_control_pending);
    mismatch(device+10496);
    refmask=before.refmask;
    require(f.guest.prepare_stencil_state(true)==4 && f.guest.stencil_state().control==control,
            "mismatch retains pending control for a successful retry");
    // The opposite partial update must also stay transactional.
    const auto control_published=f.guest.stencil_state();
    control^=0x80; refmask^=0xff00;
    f.guest.invalidate_stencil_state(sfr::GuestGraphics::stencil_refmask_pending);
    bool failed=false;
    try { f.guest.prepare_stencil_state(true); }
    catch (const sfr::RuntimeStop& error) {
        failed=error.category=="managed-stencil-state-mismatch" && error.address==device+10548;
    }
    require(failed && same_stencil(f.guest.stencil_state(),control_published),
            "pending refmask cannot conceal or partially publish past a clean control mismatch");
    control=control_published.control;
    require(f.guest.prepare_stencil_state(true)==4 && f.guest.stencil_state().refmask==refmask,
            "mismatch also retains pending refmask for retry");
    const auto reads=std::pair{control_reads,refmask_reads};
    f.guest.invalidate_stencil_state(0xFC);
    require(f.guest.prepare_stencil_state()==0 && reads==std::pair{control_reads,refmask_reads},
            "unknown notification bits cannot trigger spurious source reads");
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
                      primitive_restart_guards_publish_no_guest_or_native_effects,
                      stencil_original_setters_decode_every_face_field,
                      stencil_original_byte_setters_preserve_neighbors,
                      stencil_pending_packets_and_shared_depth_word,
                      stencil_clean_reads_and_verifier_are_independent}) {
        try { test(); } catch (const std::exception& error) { std::cerr << error.what() << '\n'; ++failures; }
    }
    if (failures) return 1;
    std::cout << "Guest renderer state tests passed\n";
    return 0;
}
