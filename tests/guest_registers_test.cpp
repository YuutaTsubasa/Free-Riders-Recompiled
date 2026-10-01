#include "guest_registers.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>

// Deliberately not a packed array: the implementation must use named fields.
struct Register { union { uint64_t u64 = 0; uint32_t u32; }; uint64_t padding = 0; };
struct Context {
    Register r0;
    Register r1;
    Register r2;
    Register r3;
    Register r4;
    Register r5;
    Register r6;
    Register r7;
    Register r8;
    Register r9;
    Register r10;
    Register r11;
    Register r12;
    Register r13;
    Register r14;
    Register r15;
    Register r16;
    Register r17;
    Register r18;
    Register r19;
    Register r20;
    Register r21;
    Register r22;
    Register r23;
    Register r24;
    Register r25;
    Register r26;
    Register r27;
    Register r28;
    Register r29;
    Register r30;
    Register r31;
    uint64_t lr = 0;
};
constexpr std::array registers{&Context::r0, &Context::r1, &Context::r2, &Context::r3, &Context::r4, &Context::r5, &Context::r6, &Context::r7, &Context::r8, &Context::r9, &Context::r10, &Context::r11, &Context::r12, &Context::r13, &Context::r14, &Context::r15, &Context::r16, &Context::r17, &Context::r18, &Context::r19, &Context::r20, &Context::r21, &Context::r22, &Context::r23, &Context::r24, &Context::r25, &Context::r26, &Context::r27, &Context::r28, &Context::r29, &Context::r30, &Context::r31};
static void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }

template<unsigned First> void valid_range(sfr::GuestMemory& memory, uint32_t sp) {
    Context ctx;
    for (unsigned i = 0; i < 32; ++i)
        (ctx.*registers[i]).u64 = 0x9876543200000000ull + 0x1020304ull * (i + 1);
    ctx.r1.u64 = sp;
    ctx.lr = 0x123456789abcdef0ull;
    const auto original = ctx;
    const uint32_t first = sp - 8 * (33 - First);
    // Sentinels protect both ends, including the unused word above saved LR.
    for (uint32_t a = first - 8; a < sp; ++a) memory.store<uint8_t>(a, 0xcc);
    require(sfr::try_save_gprs<First>(memory, ctx), "ordinary aligned stack uses grouped save");
    require(std::memcmp(&ctx, &original, sizeof(ctx)) == 0, "save preserves entire context");
    for (unsigned i = First; i < 32; ++i) {
        const uint32_t a = sp - 8 * (33 - i);
        const auto value = (original.*registers[i]).u64;
        for (unsigned b = 0; b < 8; ++b)
            require(memory.load<uint8_t>(a + b) == uint8_t(value >> (56 - 8*b)), "saved big endian GPR");
        (ctx.*registers[i]).u64 = 0;
    }
    require(memory.load<uint32_t>(sp-8) == original.r12.u32, "save uses low 32 bits of r12, not LR");
    require(memory.load<uint64_t>(first-8) == 0xccccccccccccccccull &&
            memory.load<uint32_t>(sp-4) == 0xcccccccc, "save touches only exact range");
    ctx.r12.u64 = ~uint64_t(0);
    ctx.lr = 0;
    require(sfr::try_restore_gprs<First>(memory, ctx), "ordinary aligned stack uses grouped restore");
    auto expected = original;
    expected.r12.u64 = original.r12.u32;
    expected.lr = expected.r12.u64;
    require(std::memcmp(&ctx, &expected, sizeof(ctx)) == 0, "restore preserves registers and zero extends LR");
}
template<size_t... I> void all_entries(sfr::GuestMemory& memory, std::index_sequence<I...>) {
    (valid_range<14 + I>(memory, 0x10800), ...);
}
static void rejected(sfr::GuestMemory& memory, uint32_t sp) {
    Context ctx;
    ctx.r1.u64 = sp;
    ctx.r12.u64 = 0x123456789abcdef0ull;
    const auto before = ctx;
    require(!sfr::try_save_gprs<14>(memory, ctx), "unsafe save needs instruction fallback");
    require(!sfr::try_restore_gprs<14>(memory, ctx), "unsafe restore needs instruction fallback");
    require(std::memcmp(&ctx, &before, sizeof(ctx)) == 0, "rejected fast path leaves context intact");
}
int main() {
    try {
        sfr::GuestMemory memory;
        memory.map(0x10000, 0x2000);
        all_entries(memory, std::make_index_sequence<18>{});
        rejected(memory, 0x11040); // Valid memory, but range crosses a page.
        rejected(memory, 0x10801); // Unaligned.
        rejected(memory, 8); // Would wrap below zero.
        rejected(memory, 0x30000); // Unmapped.
        memory.watch_writes(0x10000, 0x1000);
        memory.enable_write_epochs();
        memory.take_written(0x10000, 0x1000);
        memory.advance_write_epoch();
        valid_range<28>(memory, 0x10800);
        memory.take_written(0x10000, 0x1000);
        memory.advance_write_epoch();
        Context watched; watched.r1.u32 = 0x10800;
        require(sfr::try_save_gprs<28>(memory, watched), "watched stack can save");
        require(memory.take_written(0x10000, 0x1000) &&
                memory.written_since(0x10000, 0x1000, memory.write_epoch()), "grouped save publishes dirty epoch");
        memory.load_reserved_word(0x10000);
        require(!sfr::try_save_gprs<28>(memory, watched), "reservation rejects grouped stores");
        require(memory.has_reservation(), "rejection preserves reservation");
        memory.store_conditional_word(0x10000, 0);
        const sfr::GuestMemory::Range pin{0x107f0, 8};
        auto lease = memory.pin_writes(std::span(&pin, 1));
        require(!sfr::try_save_gprs<28>(memory, watched), "pending writes reject grouped stores");
        lease.reset();

        memory.map(0x20000, 0x2d8);
        valid_range<14>(memory, 0x202d8);
        rejected(memory, 0x202e0); // End exceeds exact partial commit.
        memory.map(0xfffff000, 0x1000);
        valid_range<31>(memory, 0xfffffff8);

        int provider_calls = 0;
        memory.add_read_only_word(0x107f8, [&] { ++provider_calls; return 0u; });
        const auto prior = memory.load<uint64_t>(0x10768);
        rejected(memory, 0x10800);
        require(provider_calls == 0 && memory.load<uint64_t>(0x10768) == prior,
                "declining special range does not call provider or partly write");
        memory.add_import_variable(0x117f8, "test");
        rejected(memory, 0x11800);
        std::cout << "Grouped register save/restore passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
