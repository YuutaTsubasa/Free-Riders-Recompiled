#include "native_vertex_layout.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static void word(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(offset + i) = uint8_t(value >> (24 - 8 * i));
}
static std::vector<uint8_t> declaration() {
    std::vector<uint8_t> bytes(24);
    word(bytes, 4, 0x2A23B9); // FLOAT3 POSITION0
    word(bytes, 12, 12); // Stream 0, offset 12.
    word(bytes, 16, 0x2A2187); // DEC3N NORMAL0
    bytes[21] = 3;
    return bytes;
}
int main(int argc, char** argv) {
    try {
        sfr::NativeVertexLayoutCache cache;
        auto bytes = declaration();
        const auto& first = cache.get(1, 16, bytes);
        require(first.elements.size() == 20, "all shader inputs are populated");
        require(first.elements[0].location == 0 && first.elements[0].alignedByteOffset == 0, "position binding preserved");
        require(first.dec3n_offsets == std::vector<uint32_t>{12}, "DEC3N source offset preserved");
        require(first.elements[1].alignedByteOffset == 16, "DEC3N appended after stride");
        require(cache.get(1, 16, bytes).dec3n_offsets == first.dec3n_offsets && cache.hits() == 1,
                "unchanged bytes reuse the decoded layout");
        const auto& widened = cache.get(1, 32, bytes);
        require(widened.elements[1].alignedByteOffset == 32, "stride changes invalidate repacked layout");
        word(bytes, 12, 8);
        require(cache.get(1, 16, bytes).dec3n_offsets[0] == 8, "same-address edits cannot reuse stale offsets");
        bytes.resize(12);
        require(cache.get(1, 16, bytes).dec3n_offsets.empty(), "shorter declarations remove old attributes");
        bytes[9] = 5; bytes[10] = 4;
        require(cache.get(1, 16, bytes).elements[0].location == 1, "TEXCOORD4 remaps to POSITION1");
        bytes[10] = 0; word(bytes, 4, 0x2C2359);
        require(cache.get(1, 16, bytes).swapped[5] == 1, "SHORT2 TEXCOORD sets the component swap mask");
        word(bytes, 4, 0x2A23B9);
        require(cache.get(1, 16, bytes).swapped[5] == 0, "format edits clear a stale swap mask");
        for (unsigned i = 2; i != 40; ++i) cache.get(i, 16, bytes);
        bytes = declaration();
        require(cache.get(1, 16, bytes).dec3n_offsets[0] == 12, "eviction cannot leak another declaration");
        auto rejects = [&](uint32_t stride) {
            bool threw = false;
            try { cache.get(1, stride, bytes); } catch (const std::exception&) { threw = true; }
            require(threw, "invalid input must still be rejected after a cache hit");
        };
        rejects(12); // DEC3N extends past stride.
        bytes[0] = 1; rejects(16); // Unsupported stream.
        bytes = declaration(); word(bytes, 4, 0xFFFFFFFF); rejects(16);
        bytes.resize(17 * 12); rejects(16);
        bytes.resize(13); rejects(16);
        bytes.assign(16 * 12, 0);
        for (size_t i = 0; i < 16; ++i) word(bytes, i * 12 + 4, 0x2A23B9);
        require(cache.get(2, 16, bytes).elements.size() == 35,
                "maximum declaration preserves duplicate bindings and adds missing inputs");
        const auto misses_before_invalid = cache.misses();
        word(bytes, 4, 0xFFFFFFFF); rejects(16);
        word(bytes, 4, 0x2A23B9);
        require(cache.get(2, 16, bytes).elements.size() == 35 && cache.misses() == misses_before_invalid,
                "a rejected decode leaves existing cached values intact");
        require(cache.get(1, 16, {}).elements.size() == 20, "empty layout binds default inputs");
        if (argc > 1 && std::string_view(argv[1]) == "--benchmark") {
            bytes = declaration();
            uint64_t sum = 0;
            for (bool cached : {false, true}) {
                const auto begin = std::chrono::steady_clock::now();
                for (unsigned i = 0; i != 200000; ++i) {
                    if (cached) sum += cache.get(1, 16, bytes).elements.size();
                    else sum += sfr::decode_native_vertex_layout(bytes, 16).elements.size();
                }
                std::cout << (cached ? "cached" : "decoded") << " ms="
                    << std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count() << '\n';
            }
            require(sum == 8000000, "benchmark performed the same work");
        }
        std::cout << "Native vertex layout checks passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
