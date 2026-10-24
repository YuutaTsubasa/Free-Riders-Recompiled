#include "native_vertex_layout.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
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
static void gpu_dec3n_layout_checks() {
    using F = plume::RenderFormat;
    const auto element = [](const auto& layout, uint32_t location) -> const plume::RenderInputElement& {
        const auto found = std::find_if(layout.elements.begin(), layout.elements.end(),
            [&](const auto& e) { return e.location == location; });
        require(found != layout.elements.end(), "requested input location exists");
        return *found;
    };
    const auto same = [](const auto& a, const auto& b) {
        return std::strcmp(a.semanticName,b.semanticName)==0 && a.semanticIndex==b.semanticIndex &&
            a.location==b.location && a.format==b.format && a.slotIndex==b.slotIndex &&
            a.alignedByteOffset==b.alignedByteOffset;
    };
    const auto input = [](uint32_t usage, uint32_t index, uint32_t offset, uint32_t type=0x2A2187u) {
        std::vector<uint8_t> bytes(12);
        word(bytes,0,offset); word(bytes,4,type); bytes[9]=uint8_t(usage); bytes[10]=uint8_t(index);
        return bytes;
    };
    const auto legacy_equal = [&](const auto& actual,const auto& legacy) {
        require(actual.dec3n_mask==0 && actual.dec3n_offsets==legacy.dec3n_offsets &&
                actual.swapped==legacy.swapped && actual.elements.size()==legacy.elements.size(),
                "ineligible whole declaration keeps the complete CPU layout");
        for(size_t i=0;i<legacy.elements.size();++i)require(same(actual.elements[i],legacy.elements[i]),"CPU fallback elements and duplicate order are unchanged");
    };
    sfr::NativeVertexLayoutCache cache;
    for(uint32_t bit=0;bit<9;++bit) {
        const uint32_t usage=bit<4?3:bit<8?6:7, index=bit<8?bit%4:0;
        const auto bytes=input(usage,index,4);
        const auto cpu=cache.get(7,20,bytes,false);
        const auto gpu=cache.get(7,20,bytes,true);
        require(cpu.dec3n_mask==0 && cpu.dec3n_offsets==std::vector<uint32_t>{4},
                "same-address declaration cache distinguishes CPU and GPU modes");
        require(gpu.dec3n_mask==(1u<<bit) && gpu.dec3n_offsets.empty() && gpu.elements.size()==29,
                "one supported packed field keeps raw stride and selects exactly its bit");
        for(uint32_t location=0;location<20;++location) {
            const auto& e=element(gpu,location);
            if(location==4+bit)require(e.slotIndex==15 && e.alignedByteOffset==0,
                                      "packed float semantic retains immutable zero fallback");
            else require(same(e,element(cpu,location)),"ordinary float inputs are not retyped");
        }
        for(uint32_t raw=0;raw<9;++raw) {
            const auto& e=element(gpu,20+raw);
            require(std::strcmp(e.semanticName,"SFR_DEC3N")==0 && e.semanticIndex==raw && e.format==F::R32_UINT &&
                    e.slotIndex==(raw==bit?0u:15u) && e.alignedByteOffset==(raw==bit?4u:0u),
                    "all nine uint inputs have explicit active or zero storage");
        }
        const auto hits=cache.hits();
        require(cache.get(7,20,bytes,true).dec3n_mask==gpu.dec3n_mask && cache.hits()==hits+1,
                "unchanged raw mode reuses only its immutable version");
        require(cache.get(7,20,bytes,false).dec3n_offsets==cpu.dec3n_offsets,"CPU mode remains available after raw mode");
    }
    std::vector<uint8_t> multiple;
    for(uint32_t bit=0;bit<9;++bit) {
        auto bytes=input(bit<4?3:bit<8?6:7,bit<8?bit%4:0,bit*4);
        multiple.insert(multiple.end(),bytes.begin(),bytes.end());
    }
    auto all=cache.get(9,36,multiple,true);
    require(all.dec3n_mask==0x1ff && all.dec3n_offsets.empty(),"all nine independent packed fields can share one raw layout");
    for(uint32_t bit=0;bit<9;++bit)require(element(all,20+bit).alignedByteOffset==bit*4,"field order preserves raw offsets");
    for(const auto& [bytes,stride]:std::array<std::pair<std::vector<uint8_t>,uint32_t>,5>{
        std::pair{input(0,0,0),16u},std::pair{input(5,0,0),16u},std::pair{input(3,4,0),16u},
        std::pair{input(3,0,2),16u},std::pair{input(3,0,4),18u}})
        legacy_equal(cache.get(11,stride,bytes,true),sfr::decode_native_vertex_layout(bytes,stride));
    auto duplicate=input(3,0,0);const auto repeated=input(3,0,4);
    duplicate.insert(duplicate.end(),repeated.begin(),repeated.end());
    legacy_equal(cache.get(12,16,duplicate,true),sfr::decode_native_vertex_layout(duplicate,16));
    auto mixed=input(3,0,0);const auto unsupported=input(0,0,4);
    mixed.insert(mixed.end(),unsupported.begin(),unsupported.end());
    legacy_equal(cache.get(13,16,mixed,true),sfr::decode_native_vertex_layout(mixed,16));
    auto ignored=input(3,0,0);const auto normal4=input(3,4,4);
    ignored.insert(ignored.end(),normal4.begin(),normal4.end());
    legacy_equal(cache.get(13,16,ignored,true),sfr::decode_native_vertex_layout(ignored,16));
    for(uint32_t type:{0x2A23B9u,0x1A23A6u,0x2C2359u,0x1A215Au,0x1A205Au}) {
        auto bytes=input(5,0,0,type);
        const auto cpu=sfr::decode_native_vertex_layout(bytes,16);
        const auto gpu=cache.get(14,16,bytes,true);
        require(!gpu.dec3n_mask && gpu.dec3n_offsets.empty() && cpu.swapped==gpu.swapped,"ordinary formats retain their conversion flags");
        for(const auto& e:cpu.elements)require(same(e,element(gpu,e.location)),"ordinary FLOAT/SHORT/normalized formats keep their types");
    }
    auto edit=input(3,0,0);cache.get(15,16,edit,true);const auto before=cache.misses();
    word(edit,4,0x2A23B9);
    require(cache.get(15,16,edit,true).dec3n_mask==0 && cache.misses()==before+1,
            "same-address format edit clears the raw mask");
    edit=input(3,0,8);require(element(cache.get(15,16,edit,true),20).alignedByteOffset==8,"same-address field edit cannot reuse a stale offset");
    for(uint32_t id=32;id<64;++id)cache.get(id,16,edit,true);
    require(element(cache.get(15,16,edit,true),20).alignedByteOffset==8,"eviction preserves raw mode and declaration bytes");
    cache.get(15,16,edit,true);const auto good=cache.misses();
    auto bad=edit;word(bad,4,0xffffffff);bool rejected=false;
    try{cache.get(15,16,bad,true);}catch(const sfr::NativeVertexLayoutError&){rejected=true;}
    require(rejected && element(cache.get(15,16,edit,true),20).alignedByteOffset==8 && cache.misses()==good,
            "failed GPU-mode decode preserves the previous immutable cached version");
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
        gpu_dec3n_layout_checks();
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
