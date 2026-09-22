#include "guest_graphics.h"
#include "graphics_defaults_fixture.h"
#include "guest_memory.h"
#include "native_graphics.h"
#include "native_render_state.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
constexpr uint32_t device = sfr::GuestGraphics::device_address;
constexpr uint32_t params = 0x10000000, output = params + 124;
constexpr uint32_t fetch(uint32_t slot) { return 0x480 + 24 * slot; }
constexpr uint32_t binding(uint32_t slot) { return 0x31B0 + 4 * slot; }
constexpr uint64_t mask(uint32_t slot) { return 0x80000000ull >> slot; }
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void rejects(F operation) {
    try { operation(); } catch (const sfr::RuntimeStop&) { return; }
    throw std::runtime_error("unsupported texture binding did not stop");
}
struct Fixture {
    sfr::GuestMemory memory;
    sfr::NativeGraphics graphics;
    sfr::GuestGraphics guest{memory, graphics};
    explicit Fixture(bool create = true) {
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
        if (create) create_device();
    }
    void create_device() {
        require(guest.create_device(0, 1, 0, 0, params, output) == 0, "create test native device");
        for (uint32_t slot = 0; slot < 8; ++slot) {
            for (uint32_t word = 0; word < 6; ++word)
                memory.store<uint32_t>(device + fetch(slot) + word * 4, 0xA1B2C303u + slot * 0x100 + word * 0x10);
            memory.store<uint32_t>(device + binding(slot), 0);
        }
        memory.store<uint64_t>(device + 0x18, 0xFEDCBA9876543210ull);
    }
    using Bytes = std::array<uint8_t, sfr::GuestGraphics::device_size>;
    Bytes bytes() const {
        Bytes result;
        std::copy_n(memory.base() + device, result.size(), result.begin());
        return result;
    }
    void native_bindings(uint32_t known) const {
        for (uint32_t slot = 0; slot < 8; ++slot) {
            if (known & (1u << slot)) require(guest.texture_binding(slot) == 0, "initialized slot retains known null identity");
            else rejects([&] { guest.texture_binding(slot); });
        }
        require(guest.render_state() == sfr::NativeRenderState{}, "texture binding does not initialize unrelated renderer state");
    }
    void preserved(const Bytes& before, uint32_t known) const {
        require(bytes() == before, "rejected texture setter changes no guest device bytes");
        native_bindings(known);
    }
};

void all_slots_clear_only_fetch_type_and_retain_independent_null_identity() {
    Fixture f(false);
    for (uint32_t slot = 0; slot < 8; ++slot) {
        rejects([&] { f.guest.set_texture(device, slot, 0, mask(slot)); });
        rejects([&] { f.guest.texture_binding(slot); });
    }
    f.create_device();
    f.native_bindings(0);
    // Deliberately nonsequential order detects accidental slot sharing.
    uint32_t known = 0;
    for (uint32_t slot : {7u, 0u, 5u, 2u, 6u, 1u, 4u, 3u}) {
        auto expected = f.bytes();
        expected[fetch(slot) + 3] &= 0xfc;
        f.guest.set_texture(device, slot, 0, mask(slot));
        known |= 1u << slot;
        require(f.bytes() == expected,
                "null bind clears only selected fetch dword0 type bits, preserving other20bytes, dirty, and neighboring slots");
        f.native_bindings(known);
        // Repeating a known-null bind still executes the original type clear.
        f.memory.store<uint32_t>(device + fetch(slot), 0xF1234567);
        expected = f.bytes(); expected[fetch(slot) + 3] &= 0xfc;
        f.guest.set_texture(device, slot, 0, mask(slot));
        require(f.bytes() == expected, "repeated null binding re-clears cache type without dirtying anything else");
        f.native_bindings(known);
    }
}

void invalid_device_slot_mask_and_resource_leave_both_representations_unchanged() {
    Fixture f;
    f.guest.set_texture(device, 3, 0, mask(3));
    constexpr uint32_t known = 1u << 3;
    for (uint32_t slot = 0; slot < 8; ++slot) {
        const auto before = f.bytes();
        for (uint32_t wrong_device : {0u, device + 4, sfr::GuestGraphics::color_handle}) {
            rejects([&] { f.guest.set_texture(wrong_device, slot, 0, mask(slot)); }); f.preserved(before, known);
        }
        for (uint64_t wrong_mask : {uint64_t{0}, UINT64_MAX, mask(slot) ^ 1, mask(slot) << 32,
                                    mask(slot) | (uint64_t{1} << 63)}) {
            rejects([&] { f.guest.set_texture(device, slot, 0, wrong_mask); }); f.preserved(before, known);
        }
        for (uint32_t resource : {1u, 0xDEADBEEFu, sfr::GuestGraphics::color_handle}) {
            rejects([&] { f.guest.set_texture(device, slot, resource, mask(slot)); }); f.preserved(before, known);
            f.memory.store<uint32_t>(device + binding(slot), resource);
            const auto old_binding = f.bytes();
            rejects([&] { f.guest.set_texture(device, slot, 0, mask(slot)); }); f.preserved(old_binding, known);
            f.memory.store<uint32_t>(device + binding(slot), 0);
        }
    }
    const auto before = f.bytes();
    for (uint32_t slot : {8u, 31u, 32u, 64u, UINT32_MAX}) {
        rejects([&] { f.guest.set_texture(device, slot, 0, 0x80000000ull); }); f.preserved(before, known);
        rejects([&] { f.guest.texture_binding(slot); });
    }
}

void both_outputs_are_preflighted_before_guest_or_native_publication() {
    for (uint32_t offset : {fetch(4), binding(4)}) {
        for (bool provider : {false, true}) {
            Fixture f;
            f.guest.set_texture(device, 1, 0, mask(1));
            const auto before = f.bytes();
            unsigned samples = 0;
            if (provider) f.memory.add_read_only_word(device + offset, [&] { ++samples; return 0u; });
            else f.memory.add_import_variable(device + offset, "TextureOutputGuard");
            rejects([&] { f.guest.set_texture(device, 4, 0, mask(4)); });
            f.preserved(before, 1u << 1);
            require(samples == 0, "output guards are checked before source reads or partial publication");
        }
    }
    Fixture f;
    f.guest.set_texture(device, 1, 0, mask(1));
    const auto before = f.bytes();
    const auto reserved = f.memory.load_reserved_word(params);
    rejects([&] { f.guest.set_texture(device, 4, 0, mask(4)); });
    f.preserved(before, 1u << 1);
    require(f.memory.has_reservation() && f.memory.store_conditional_word(params, reserved),
            "failed texture bind preserves live atomic reservation and later conditional store");
}

void null_binding_never_accesses_texture_dirty_word() {
    for (bool provider : {false, true}) {
        Fixture f;
        unsigned samples = 0;
        for (uint32_t offset : {0x18u, 0x1cu}) {
            if (provider) f.memory.add_read_only_word(device + offset, [&]() -> uint32_t {
                ++samples; throw std::logic_error("null path sampled the texture dirty word");
            });
            else f.memory.add_import_variable(device + offset, "UnusedTextureDirty");
        }
        for (uint32_t slot = 0; slot < 8; ++slot) {
            auto expected = f.bytes(); expected[fetch(slot) + 3] &= 0xfc;
            f.guest.set_texture(device, slot, 0, mask(slot));
            require(f.bytes() == expected && samples == 0,
                    "null binding succeeds without reading, writing, or requiring writable texture dirty state");
        }
        f.native_bindings(0xff);
    }
}
}

void real_texture_merges_the_original_fetch_constant_and_retires_the_old_one() {
    Fixture f;
    constexpr uint32_t texture = params + 128;  // 52-byte header inside the mapped block
    const sfr::FetchWords words{0xFFFFFFFF, 0x4006F886, 0x12345678, 0, 0xFFFF024C, 0xE0100E01};
    for (uint32_t i = 0; i < 6; ++i) f.memory.store<uint32_t>(texture + 28 + i * 4, words[i]);
    f.memory.store<uint8_t>(device + 0x2F5E + 2, 5);
    f.memory.store<uint8_t>(device + 0x2F78 + 2, 7);
    sfr::FetchWords slot;
    for (uint32_t i = 0; i < 6; ++i) slot[i] = f.memory.load<uint32_t>(device + fetch(2) + i * 4);
    const auto expected = sfr::merge_texture_fetch(words, slot, 5, 7);
    const uint64_t dirty = f.memory.load<uint64_t>(device + 0x18);
    auto untouched = f.bytes();

    f.guest.set_texture(device, 2, texture, mask(2));
    for (uint32_t i = 0; i < 6; ++i)
        require(f.memory.load<uint32_t>(device + fetch(2) + i * 4) == expected[i], "slot holds the merged fetch constant");
    require(f.memory.load<uint64_t>(device + 0x18) == (dirty | mask(2)), "texture dirty bit is set");
    require(f.memory.load<uint32_t>(device + binding(2)) == texture && f.guest.texture_binding(2) == texture,
            "binding published in both representations");
    auto after = f.bytes();
    for (uint32_t offset : {fetch(2), 0x18u, binding(2)})
        for (uint32_t i = 0; i < (offset == fetch(2) ? 24u : offset == 0x18u ? 8u : 4u); ++i)
            untouched[offset + i] = after[offset + i];
    require(after == untouched, "no other device byte changes");

    // Replacing it while the device fence (+0x2A9C) is set stamps the old texture.
    f.memory.store<uint32_t>(device + 0x2A9C, 0x77);
    f.guest.set_texture(device, 2, 0, mask(2));
    require(f.memory.load<uint32_t>(texture + 8) == 0x77 && f.guest.texture_binding(2) == 0,
            "old texture receives the current fence");

    // Without a fence, a texture flagged by +0x2AA0 would be queued: unsupported.
    f.guest.set_texture(device, 2, texture, mask(2));
    f.memory.store<uint32_t>(device + 0x2A9C, 0);
    f.memory.store<uint32_t>(device + 0x2AA0, 1);
    f.memory.store<uint32_t>(texture, 1);
    const auto before = f.bytes();
    rejects([&] { f.guest.set_texture(device, 2, 0, mask(2)); });
    require(f.bytes() == before && f.guest.texture_binding(2) == texture, "queued retirement stops unchanged");
    // Unflagged, it simply leaves the old texture alone.
    f.memory.store<uint32_t>(texture, 0);
    f.guest.set_texture(device, 2, 0, mask(2));
    require(f.memory.load<uint32_t>(texture + 8) == 0x77 && f.guest.texture_binding(2) == 0, "unflagged replacement");
}

int main() {
    unsigned failures = 0;
    for (auto test : {real_texture_merges_the_original_fetch_constant_and_retires_the_old_one,
                      all_slots_clear_only_fetch_type_and_retain_independent_null_identity,
                      invalid_device_slot_mask_and_resource_leave_both_representations_unchanged,
                      both_outputs_are_preflighted_before_guest_or_native_publication,
                      null_binding_never_accesses_texture_dirty_word}) {
        try { test(); } catch (const std::exception& error) { std::cerr << error.what() << '\n'; ++failures; }
    }
    if (failures) return 1;
    std::cout << "Guest texture binding tests passed\n";
    return 0;
}
