#include "graphics_defaults_fixture.h"
#include "guest_graphics.h"
#include "guest_memory.h"
#include "native_blend_control.h"
#include "native_graphics.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
constexpr uint32_t device = sfr::GuestGraphics::device_address;
constexpr uint32_t params = 0x10000000;
constexpr uint32_t output = params + 124;
constexpr std::array<uint32_t, 4> target_offsets{0x2938, 0x2958, 0x295C, 0x2960};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void rejects(F&& operation, const char* message) {
    try { operation(); }
    catch (const sfr::RuntimeStop&) { return; }
    throw std::runtime_error(message);
}

struct Fixture {
    sfr::GuestMemory memory;
    sfr::NativeGraphics graphics;
    sfr::GuestGraphics guest{memory, graphics};
    using Bytes = std::array<uint8_t, sfr::GuestGraphics::device_size>;

    Fixture() {
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
        require(guest.create_device(0, 1, 0, 0, params, output) == 0, "create device");
    }
    Bytes bytes() const {
        Bytes result{};
        std::copy_n(memory.base() + device, result.size(), result.begin());
        return result;
    }
    std::array<sfr::NativeBlendControl, 4> controls() const {
        return {guest.blend_control(0), guest.blend_control(1), guest.blend_control(2), guest.blend_control(3)};
    }
};

void write_be(Fixture::Bytes& bytes, uint32_t offset, uint64_t value, uint32_t width) {
    for (uint32_t i = 0; i < width; ++i) bytes[offset + i] = uint8_t(value >> ((width - i - 1) * 8));
}

uint32_t mirrored_alpha(uint32_t requested) {
    return (requested & 0xFFFF) |
           (((requested << 16) | ((requested & 0x1010) << 12)) & 0xEFEF0000);
}

void defaults_and_enable_full_dword_semantics() {
    Fixture f;
    require(f.memory.load<uint32_t>(device + 0x2EF8) == 0x00010001 &&
            f.memory.load<uint32_t>(device + 0x2EFC) == 0x00240000,
            "creation installs independently proven requested blend defaults");
    for (uint32_t offset : target_offsets)
        require(f.memory.load<uint32_t>(device + offset) == 0x00010001, "creation installs four copy controls");

    f.memory.store<uint64_t>(device + 16, 0x8123456700000000ull);
    auto expected = f.bytes();
    write_be(expected, 0x2EFC, 0x80240000, 4);
    for (uint32_t offset : target_offsets) write_be(expected, offset, 0x00010001, 4);
    write_be(expected, 16, 0x8123456700000407ull, 8);
    f.guest.set_blend_request(device, sfr::BlendRequest::enable, 1);
    require(f.bytes() == expected, "enable mutates exact requested flag, four targets and dirty word");
    require(f.controls() == std::array<sfr::NativeBlendControl, 4>{sfr::decode_blend_control(0x00010001),
            sfr::decode_blend_control(0x00010001), sfr::decode_blend_control(0x00010001),
            sfr::decode_blend_control(0x00010001)}, "enable publishes all four effective native controls");

    f.memory.store<uint64_t>(device + 16, 0);
    f.guest.set_blend_request(device, sfr::BlendRequest::enable, 1);
    require(f.memory.load<uint64_t>(device + 16) == 0x407, "repeat enable still marks exact original dirty bits");

    f.memory.store<uint32_t>(device + 0x2EF8, 0x07010706);
    f.memory.store<uint32_t>(device + 0x2EFC, 0xFFFFFFFF);
    f.memory.store<uint64_t>(device + 16, 0);
    f.guest.set_blend_request(device, sfr::BlendRequest::enable, 2);
    require(f.memory.load<uint32_t>(device + 0x2EFC) == 0x7FFFFFFF,
            "noncanonical two clears requested bit31 through V bit zero");
    for (uint32_t offset : target_offsets)
        require(f.memory.load<uint32_t>(device + offset) == 0x07010706,
                "nonzero two still takes effective blend publication branch");
    require(f.memory.load<uint64_t>(device + 16) == 0x407,
            "noncanonical nonzero enable retains full DWORD branch semantics");

    f.guest.set_blend_request(device, sfr::BlendRequest::enable, 0);
    require(f.memory.load<uint32_t>(device + 0x2EFC) == 0x7FFFFFFF,
            "disable preserves every unrelated requested flag bit");
    for (uint32_t offset : target_offsets)
        require(f.memory.load<uint32_t>(device + offset) == 0x00010001, "disable publishes copy to every target");
}

void factor_updates_and_separate_alpha() {
    Fixture f;
    auto before_controls = f.controls();
    f.memory.store<uint64_t>(device + 16, 0x1234000000000000ull);
    f.guest.set_blend_request(device, sfr::BlendRequest::source, 6);
    require(f.memory.load<uint32_t>(device + 0x2EF8) == 0x00010006 &&
            f.memory.load<uint64_t>(device + 16) == 0x1234000000000000ull && f.controls() == before_controls,
            "disabled source changes requested bits only");
    for (uint32_t offset : target_offsets)
        require(f.memory.load<uint32_t>(device + offset) == 0x00010001, "disabled source leaves effective targets alone");
    f.guest.set_blend_request(device, sfr::BlendRequest::destination, 7);
    require(f.memory.load<uint32_t>(device + 0x2EF8) == 0x00010706 && f.controls() == before_controls,
            "disabled destination masks to five bits and preserves other requested fields");
    f.guest.set_blend_request(device, sfr::BlendRequest::destination, 0x80000007);
    require(f.memory.load<uint32_t>(device + 0x2EF8) == 0x00010706,
            "destination ignores every bit above its five-bit field");

    f.guest.set_blend_request(device, sfr::BlendRequest::enable, 1);
    const uint32_t effective = mirrored_alpha(0x00010706);
    require(effective == 0x07060706, "independent oracle derives non-separate alpha half");
    for (uint32_t offset : target_offsets)
        require(f.memory.load<uint32_t>(device + offset) == effective, "enabled request rebuilds every target");
    for (const auto& control : f.controls()) require(control == sfr::decode_blend_control(effective),
                                                      "native target matches rebuilt control");

    f.memory.store<uint32_t>(device + 0x2EF8, 0x07010706);
    f.memory.store<uint32_t>(device + 0x2EFC, 0xC02455AA);
    f.guest.set_blend_request(device, sfr::BlendRequest::destination, 7);
    require(f.memory.load<uint32_t>(device + 0x2EF8) == 0x07010706 &&
            f.memory.load<uint32_t>(device + 0x2EFC) == 0xC02455AA,
            "factor mask preserves requested operation, alpha and flag fields");
    for (uint32_t offset : target_offsets)
        require(f.memory.load<uint32_t>(device + offset) == 0x07010706,
                "separate-alpha flag publishes requested packed control without reconstruction");

    f.guest.set_blend_request(device, sfr::BlendRequest::source, 0xFFFFFFE6);
    require((f.memory.load<uint32_t>(device + 0x2EF8) & 31) == 6,
            "source factor consumes exactly its low five bits");

    // 824E6AD0 (BLENDOP) inserts value & 7 at bits 5..7 and otherwise matches
    // the destination setter.
    Fixture operation;
    const auto operation_controls = operation.controls();
    operation.memory.store<uint64_t>(device + 16, 0x1234000000000000ull);
    operation.guest.set_blend_request(device, sfr::BlendRequest::operation, 0xFFFFFFF1);
    require(operation.memory.load<uint32_t>(device + 0x2EF8) == 0x00010021 &&
            operation.memory.load<uint64_t>(device + 16) == 0x1234000000000000ull &&
            operation.controls() == operation_controls,
            "disabled operation changes only its three requested bits");
    operation.guest.set_blend_request(device, sfr::BlendRequest::enable, 1);
    const uint32_t subtract = mirrored_alpha(0x00010021);
    for (uint32_t offset : target_offsets)
        require(operation.memory.load<uint32_t>(device + offset) == subtract, "enabled operation reaches every target");
    operation.guest.set_blend_request(device, sfr::BlendRequest::operation, 0);
    require(operation.memory.load<uint32_t>(device + 0x2EF8) == 0x00010001, "operation reset keeps factors");
    for (uint32_t offset : target_offsets)
        require(operation.memory.load<uint32_t>(device + offset) == mirrored_alpha(0x00010001),
                "enabled operation setter republishes the effective control");
    for (const auto& control : operation.controls())
        require(control == sfr::decode_blend_control(mirrored_alpha(0x00010001)), "native control follows operation");

    Fixture saturate;
    saturate.memory.store<uint32_t>(device + 0x2EF8, 0x00100010);
    require(mirrored_alpha(0x00100010) == 0x00010010,
            "source factor bit16 reconstructs alpha ONE rather than alpha SRC_ALPHA_SAT");
    saturate.guest.set_blend_request(device, sfr::BlendRequest::enable, 1);
    for (uint32_t offset : target_offsets)
        require(saturate.memory.load<uint32_t>(device + offset) == 0x00010010,
                "bit16 factor uses the exact original reconstructed alpha field");
}

void failures_are_preflighted_and_atomic() {
    for (uint32_t guarded : {0x2960u, 0x10u, 0x2EFCu, 0x2EF8u}) {
        Fixture f;
        if (guarded == 0x2EF8) f.memory.store<uint32_t>(device + 0x2EFC, 0x80240000);
        const auto bytes = f.bytes();
        const auto controls = f.controls();
        f.memory.add_read_only_word(device + guarded, [] { return 0u; });
        const auto request = guarded == 0x2EF8 ? sfr::BlendRequest::source : sfr::BlendRequest::enable;
        rejects([&] { f.guest.set_blend_request(device, request, 1); },
                "guarded blend bundle must reject");
        require(f.bytes() == bytes && f.controls() == controls,
                "late target, dirty and request guards preserve complete guest and native state");
    }

    Fixture readable_requested;
    readable_requested.memory.add_read_only_word(device + 0x2EF8, [] { return 0x00010001u; });
    readable_requested.guest.set_blend_request(device, sfr::BlendRequest::enable, 1);
    require(readable_requested.memory.load<uint32_t>(device + 0x2EFC) == 0x80240000,
            "enable accepts requested blend state supplied by a read-only provider");
    for (uint32_t offset : target_offsets)
        require(readable_requested.memory.load<uint32_t>(device + offset) == 0x00010001 &&
                readable_requested.guest.blend_control((offset == 0x2938) ? 0 :
                    (offset == 0x2958) ? 1 : (offset == 0x295C) ? 2 : 3) ==
                    sfr::decode_blend_control(0x00010001),
                "provider-backed requested state publishes matching effective guest and native controls");

    Fixture inactive;
    const auto inactive_controls = inactive.controls();
    for (uint32_t offset : target_offsets)
        inactive.memory.add_read_only_word(device + offset, []() -> uint32_t {
            throw std::runtime_error("disabled factor must not read effective target");
        });
    inactive.memory.add_read_only_word(device + 0x10, []() -> uint32_t {
        throw std::runtime_error("disabled factor must not read dirty word");
    });
    inactive.guest.set_blend_request(device, sfr::BlendRequest::source, 6);
    require(inactive.memory.load<uint32_t>(device + 0x2EF8) == 0x00010006 &&
            inactive.controls() == inactive_controls,
            "disabled factor update skips guarded effective targets and dirty word entirely");

    Fixture invalid;
    invalid.memory.store<uint32_t>(device + 0x2EF8, 0x0001000E);
    const auto bytes = invalid.bytes();
    const auto controls = invalid.controls();
    rejects([&] { invalid.guest.set_blend_request(device, sfr::BlendRequest::enable, 1); },
            "unsupported decoded factor rejects bundle");
    require(invalid.bytes() == bytes && invalid.controls() == controls,
            "decode failure precedes requested, effective and dirty publication");

    Fixture reservation;
    const auto reservation_bytes = reservation.bytes();
    const auto reservation_controls = reservation.controls();
    const uint32_t saved = reservation.memory.load_reserved_word(params);
    rejects([&] { reservation.guest.set_blend_request(device, sfr::BlendRequest::enable, 1); },
            "live reservation rejects blend publication");
    require(reservation.bytes() == reservation_bytes && reservation.controls() == reservation_controls &&
            reservation.memory.store_conditional_word(params, saved),
            "failed blend request preserves state and reservation usability");

    rejects([&] { reservation.guest.set_blend_request(device + 4, sfr::BlendRequest::enable, 1); },
            "wrong device rejects");
    rejects([&] { reservation.guest.set_blend_request(device, static_cast<sfr::BlendRequest>(0x44), 1); },
            "unknown request code rejects");
}
}

int main() {
    try {
        defaults_and_enable_full_dword_semantics();
        factor_updates_and_separate_alpha();
        failures_are_preflighted_and_atomic();
        std::cout << "guest blend request tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
