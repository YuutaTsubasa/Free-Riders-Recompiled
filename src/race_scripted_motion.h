#pragma once
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
namespace sfr {
template<class Memory,class Invoke>
void with_scripted_force_step(Memory& memory,uint32_t physics,float frames,Invoke&& invoke) {
    // Motion mode 1 adds the supplied vector directly in 822AB3B0, while its
    // gravity displacement already includes elapsed frames. Convert the input
    // only for this consumer; retain the producer's per-frame velocity afterward.
    if(frames==1.0f || !std::isfinite(frames) || frames<=0.0f ||
       memory.template load<uint32_t>(uint64_t(physics)+2392)!=1) {
        invoke();return;
    }
    std::array<uint32_t,3> saved{},scaled{};
    for(uint32_t i=0;i<3;++i) {
        saved[i]=memory.template load<uint32_t>(uint64_t(physics)+256+i*4);
        const float value=std::bit_cast<float>(saved[i]);
        const float displacement=value*frames;
        if(!std::isfinite(value) || !std::isfinite(displacement)) {invoke();return;}
        scaled[i]=std::bit_cast<uint32_t>(displacement);
    }
    for(uint32_t i=0;i<3;++i)
        memory.template store<uint32_t>(uint64_t(physics)+256+i*4,scaled[i]);
    const auto restore=[&] {
        for(uint32_t i=0;i<3;++i) {
            const uint64_t address=uint64_t(physics)+256+i*4;
            // Keep intentional original-handler changes, including state exits.
            if(memory.template load<uint32_t>(address)==scaled[i])
                memory.template store<uint32_t>(address,saved[i]);
        }
    };
    try {invoke();} catch(...) {restore();throw;}
    restore();
}
template<class Memory,class Invoke>
void with_scripted_path_update(Memory& memory,uint32_t object,Invoke&& invoke) {
    const uint32_t rider=memory.template load<uint32_t>(object);
    const uint32_t route=memory.template load<uint32_t>(uint64_t(object)+228);
    if(!rider || route>=2) {invoke();return;}
    // The two original route records have a 36-byte stride. 8231CA00 checks
    // the previous node for equality; advancing from 143 to 149 skips node 144.
    const uint16_t node=memory.template load<uint16_t>(uint64_t(object)+90);
    const uint32_t end=memory.template load<uint32_t>(0x821848B8ull+route*36+32);
    const bool missed=node>end && memory.template load<uint8_t>(uint64_t(object)+248)!=0;
    invoke();
    if(missed && memory.template load<uint32_t>(object)==rider &&
       memory.template load<uint32_t>(uint64_t(object)+228)==route &&
       memory.template load<uint16_t>(uint64_t(object)+90)>=node &&
       memory.template load<uint8_t>(uint64_t(object)+248)!=0)
        memory.template store<uint8_t>(uint64_t(object)+248,0);
}
}
