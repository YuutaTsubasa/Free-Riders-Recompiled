#include "guest_memory.h"
#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class F> void rejects(F action) {
    try { action(); } catch (const sfr::RuntimeStop&) { return; }
    throw std::runtime_error("pending memory operation should reject before changing guest state");
}
using Range = sfr::GuestMemory::Range;
void precise_owner_and_atomic_admission() {
    sfr::GuestMemory memory;
    memory.map(0x10000, 0x3000);
    memory.store<uint64_t>(0x10100, 0x1122334455667788ull);
    const std::array outputs{Range{0x10100, 4}, Range{0x11000, 8}};
    auto token=memory.pin_writes(outputs);
    token.check();
    require(memory.load<uint64_t>(0x10100)==0x1122334455667788ull, "pinning does not change guest bytes or deny reads");
    rejects([&]{ memory.store<uint8_t>(0x10100, 1); });
    rejects([&]{ memory.store<uint32_t>(0x100ff, 1); });
    rejects([&]{ memory.store<uint32_t>(0x11004, 1); });
    rejects([&]{ (void)memory.pin_writes(outputs); });
    rejects([&]{ token.store<uint32_t>(0x10102, 1); });
    rejects([&]{ token.store<uint8_t>(0x10104, 1); });
    token.store<uint32_t>(0x10100, 0xaabbccdd);
    token.store<uint32_t>(0x11000, 0x103);
    token.store<uint32_t>(0x11004, 4);
    require(memory.load<uint64_t>(0x10100)==0xaabbccdd55667788ull &&
            memory.load<uint64_t>(0x11000)==0x0000010300000004ull,
            "authorized publication uses big-endian stores and exact output boundaries");
    memory.store<uint8_t>(0x100ff, 0x71);
    memory.store<uint8_t>(0x11008, 0x72);
    token.reset();
    rejects([&]{ token.check(); });
    memory.store<uint32_t>(0x10100, 1);
    const std::array bad_second{Range{0x10100,4}, Range{0x20000,8}};
    rejects([&]{ (void)memory.pin_writes(bad_second); });
    memory.store<uint32_t>(0x10100, 2);
    const std::array overlap{Range{0x10100,8}, Range{0x10104,8}};
    rejects([&]{ (void)memory.pin_writes(overlap); });
    const std::array wrapping{Range{0xfffffffcul,8}};
    rejects([&]{ (void)memory.pin_writes(wrapping); });
    rejects([&]{ (void)memory.pin_writes({}); });
    const std::array zero{Range{0x10100,0}};
    rejects([&]{ (void)memory.pin_writes(zero); });
    memory.store<uint32_t>(0x10100, 3);
}
void mappings_providers_cache_and_atomics_are_protected() {
    sfr::GuestMemory memory;
    memory.map(0x10000, 0x3000);
    memory.store<uint32_t>(0x11004, 0x12345678);
    const std::array output{Range{0x11004,8}};
    auto token=memory.pin_writes(output);
    const auto usage=memory.usage();
    rejects([&]{ memory.decommit(0x11000,0x1000); });
    rejects([&]{ memory.release(0x10000,0x3000); });
    rejects([&]{ memory.commit(0x11000,0x1000); });
    rejects([&]{ memory.add_read_only_word(0x11004, []{return 7u;}); });
    rejects([&]{ memory.add_import_variable(0x11008, "pending"); });
    rejects([&]{ memory.zero_cache_block(0x11001); });
    rejects([&]{ memory.zero_cache_line(0x11070); });
    rejects([&]{ (void)memory.load_reserved_word(0x11004); });
    rejects([&]{ (void)memory.store_conditional_doubleword(0x11000, 5); });
    require(!memory.has_reservation() && memory.usage().committed_bytes==usage.committed_bytes &&
            memory.usage().reserved_bytes==usage.reserved_bytes && memory.load<uint32_t>(0x11004)==0x12345678,
            "pending conflicts preserve mappings, bytes, providers and reservation state");
    memory.decommit(0x12000,0x1000);
    token.check();
    token.store<uint32_t>(0x11004, 0x90abcdef);
    memory.load_reserved_word(0x10000);
    rejects([&]{ token.store<uint32_t>(0x11004, 1); });
    rejects([&]{ (void)memory.pin_writes(std::array{Range{0x10010,4}}); });
    require(memory.store_conditional_word(0x10000, 1), "failed pending admission preserves live reservation");
    token.reset();
    memory.decommit(0x11000,0x1000);
    memory.commit(0x11000,0x1000);
    memory.store<uint32_t>(0x11004, 9);
    memory.release(0x10000,0x3000);
}
void independent_tokens_move_and_owner_lifetime() {
    auto memory=std::make_unique<sfr::GuestMemory>();
    memory->map(0x10000,0x1000);
    auto first=memory->pin_writes(std::array{Range{0x10000,4}});
    auto second=memory->pin_writes(std::array{Range{0x10004,4}});
    rejects([&]{ first.store<uint32_t>(0x10004, 1); });
    auto moved=std::move(first);
    rejects([&]{ first.check(); });
    moved.store<uint32_t>(0x10000, 2);
    second=std::move(moved);
    memory->store<uint32_t>(0x10004, 3);
    rejects([&]{ memory->store<uint32_t>(0x10000, 3); });
    second.store<uint32_t>(0x10000,4);
    memory.reset();
    rejects([&]{ second.check(); });
    rejects([&]{ second.store<uint8_t>(0x10000,1); });
    second.reset();
}
}
int main() {
    unsigned failed=0;
    for (auto test : {precise_owner_and_atomic_admission, mappings_providers_cache_and_atomics_are_protected,
                      independent_tokens_move_and_owner_lifetime}) {
        try { test(); } catch (const std::exception& error) { std::cerr << error.what() << '\n'; ++failed; }
    }
    return failed ? 1 : 0;
}
