#include "native_vertex_layout.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <mutex>
#include <set>

namespace sfr {
NativeVertexLayout decode_native_vertex_layout(std::span<const uint8_t> bytes, uint32_t stride) {
    if (bytes.size() > 192 || bytes.size() % 12)
        throw NativeVertexLayoutError(uint32_t(bytes.size()), "invalid vertex declaration byte count");
    NativeVertexLayout result;
    result.elements.reserve(36);
    const uint32_t elements = uint32_t(bytes.size()/12);
    const auto read = [&](size_t offset, size_t size) {
        uint32_t value = 0;
        for (size_t i=0; i<size; ++i) value = (value << 8) | bytes[offset+i];
        return value;
    };
    // Locations must match XenosRecomp (as in Marathon Recompiled).
    struct Location { uint32_t usage, index; };
    constexpr Location locations[]={{0,0},{0,1},{0,2},{0,3},{3,0},{3,1},{3,2},{3,3},{6,0},{6,1},{6,2},{6,3},
                                    {7,0},{5,0},{5,1},{5,2},{5,3},{10,0},{2,0},{1,0}};
    for(uint32_t i=0;i<elements;++i) {
        const size_t e=12*i;
        const uint32_t stream=read(e,2), offset=read(e+2,2);
        const uint32_t type=read(e+4,4), usage=read(e+9,1), index=read(e+10,1);
        constexpr uint32_t dec3n=0x2A2187;
        const auto format=type==dec3n
            ? std::optional<sfr::DeclarationFormat>(sfr::DeclarationFormat{plume::RenderFormat::R16G16B16A16_SNORM,false})
            : sfr::declaration_format(type);
        const auto input=sfr::shader_input_usage(usage,index);
        const char* semantic=sfr::declaration_semantic(input.usage);
        if(stream!=0 || !format || !semantic)
            throw NativeVertexLayoutError(type,"unsupported vertex element for an up draw");
        uint32_t location=~0u;
        for(uint32_t l=0;l<std::size(locations);++l)
            if(locations[l].usage==input.usage && locations[l].index==input.index) location=l;
        if(location==~0u) continue;  // bound but read by no XenosRecomp shader
        uint32_t element_offset=offset;
        if(type==dec3n) {
            if(offset+4>stride) throw NativeVertexLayoutError(offset,"vertex element exceeds the stride");
            element_offset=stride+8*uint32_t(result.dec3n_offsets.size());
            result.dec3n_offsets.push_back(offset);
        }
        // Vulkan wants an attribute's offset aligned to its component size.
        // Desktop drivers fetch an under-aligned one anyway; an Adreno need
        // not, which would draw some meshes as scattered triangles. Reported
        // once per (type, offset) so a device's log says whether that is it.
        if(const uint32_t component=sfr::format_component_bytes(format->format);
           component>1 && element_offset%component) {
            static std::set<std::pair<uint32_t,uint32_t>> misaligned;
            static std::mutex misaligned_lock;
            std::lock_guard guard(misaligned_lock);
            if(misaligned.size()<32 && misaligned.insert({type,element_offset}).second)
                std::cerr << "NATIVE_VERTEX_MISALIGNED type=0x" << std::hex << type << " offset=" << std::dec
                          << element_offset << " component=" << component << " stride=" << stride
                          << " semantic=" << semantic << input.index << '\n';
        }
        result.elements.emplace_back(semantic,input.index,location,format->format,0,element_offset);
        if(format->swapped_pairs && (input.usage==1 || input.usage==3 || input.usage==5 || input.usage==6 || input.usage==7)) result.swapped[input.usage]|=1u<<input.index;
    }
    for(uint32_t l=0;l<std::size(locations);++l) {
        if(std::any_of(result.elements.begin(),result.elements.end(),[&](const auto& e){ return e.location==l; })) continue;
        const uint32_t usage=locations[l].usage;
        const bool vector3=usage==3 || usage==6 || usage==7 || usage==2;
        result.elements.emplace_back(sfr::declaration_semantic(usage),locations[l].index,l,
            vector3?plume::RenderFormat::R32G32B32_FLOAT:plume::RenderFormat::R32_FLOAT,15,0);
    }

    return result;
}

const NativeVertexLayout& NativeVertexLayoutCache::get(uint32_t identity, uint32_t stride,
                                                       std::span<const uint8_t> bytes) {
    if (bytes.size() > 192 || bytes.size() % 12)
        throw NativeVertexLayoutError(uint32_t(bytes.size()), "invalid vertex declaration byte count");
    // Guest memory can change while a detached worker runs. Compare, decode
    // and remember ONE snapshot, never a decoded value from one read paired
    // with a cache key reread after it. Keep these guest reads observable.
    std::array<uint8_t, 192> snapshot;
    const volatile uint8_t* source = bytes.data();
    for (size_t i=0; i<bytes.size(); ++i) snapshot[i] = source[i];
    bytes = {snapshot.data(), bytes.size()};
    const auto matches = [&](const Entry& entry) {
        return entry.valid && entry.identity == identity && entry.stride == stride && entry.size == bytes.size() &&
            (bytes.empty() || std::memcmp(entry.bytes.data(), bytes.data(), bytes.size()) == 0);
    };
    for (size_t offset=0; offset<entries_.size(); ++offset) {
        const size_t index = (last_+offset)%entries_.size();
        if (matches(entries_[index])) {
            last_ = index;
            ++hits_;
            return entries_[index].layout;
        }
    }
    // Decode before replacing anything: failure leaves the old entries valid.
    auto decoded = decode_native_vertex_layout(bytes, stride);
    auto& entry = entries_[next_];
    entry.layout = std::move(decoded);
    std::copy(bytes.begin(), bytes.end(), entry.bytes.begin());
    entry.identity = identity; entry.stride = stride; entry.size = bytes.size(); entry.valid = true;
    last_ = next_; next_ = (next_+1)%entries_.size();
    ++misses_;
    return entry.layout;
}
}
