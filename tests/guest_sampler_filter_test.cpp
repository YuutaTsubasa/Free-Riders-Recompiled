#include "graphics_defaults_fixture.h"
#include "guest_graphics.h"
#include "guest_memory.h"
#include "native_graphics.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
constexpr uint32_t device=sfr::GuestGraphics::device_address, params=0x10000000, output=params+124;
constexpr uint32_t lookup=0x82001608;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
template<class F> void rejects(F f,const char* why){try{f();}catch(const sfr::RuntimeStop&){return;}throw std::runtime_error(why);}

struct Fixture {
    sfr::GuestMemory memory; sfr::NativeGraphics graphics; sfr::GuestGraphics guest{memory,graphics};
    using Bytes=std::array<uint8_t,sfr::GuestGraphics::device_size>;
    Fixture(){
        memory.map(params,256);
        constexpr std::array<uint32_t,31> words{19,11,0x18280186,1,0,0,1,0,0,1,0x1A220197,0,0,1,0,0,
            0x28280106,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
        for(size_t i=0;i<words.size();++i)memory.store<uint32_t>(params+i*4,words[i]);
        memory.map(0x82AD0000,0x2000);
        for(auto [table,count]:{std::pair{0x82AD0A60u,101u},std::pair{0x82AD0F20u,20u}})
            for(uint32_t i=0;i<count;++i){memory.store<uint32_t>(table+i*12,0x12340000+i);
                memory.store<uint32_t>(table+i*12+4,0x82210000+i*4);memory.store<uint32_t>(table+i*12+8,i);}
        sfr::test::install_blend_defaults(memory); sfr::test::install_sampler_defaults(memory);
        require(guest.create_device(0,1,0,0,params,output)==0,"create device");
    }
    Bytes bytes()const{Bytes r{};std::copy_n(memory.base()+device,r.size(),r.begin());return r;}
};

uint32_t rotl(uint32_t x,unsigned n){return std::rotl(x,int(n));}
sfr::SamplerFilterState decode(uint32_t w3,uint32_t w4){return {w3,w4,uint8_t((w3>>19)&3),uint8_t((w3>>21)&3),
    uint8_t((w3>>23)&3),uint8_t((w3>>25)&7),uint8_t(w4&1),uint8_t((w4>>1)&1),bool(w4&0x400),bool(w4&0x800)};}
sfr::SamplerFilterState oracle(uint32_t w3,uint32_t w4,uint32_t a,uint8_t c,uint32_t v,bool mag){
    uint32_t q=v>>2,other=(w4>>(mag?11:10))&1;
    uint32_t w4a=(w4&~(mag?0x400u:0x800u))|((q<<(mag?10:11))&(mag?0x400u:0x800u));
    uint32_t x=((a&~((other|q)-1u))<<(mag?6:4))|q|v;
    uint32_t mask=mag?0x0E180000u:0x0E600000u;
    uint32_t w3a=(w3&~mask)|(rotl(x,mag?19:21)&mask);
    uint32_t t=(w3a&~0x0007FFFFu)|(rotl(w3a,31)&0x0007FFFFu);
    t=(t&~0x7FF00000u)|(rotl(w3a,31)&0x7FF00000u);
    uint32_t m=uint32_t(c>>2)-1u,vf=((rotl(t,13)&0xFFF)&m)+(uint32_t(c)&~m);
    return decode(w3a,(w4a&0xFFFFFFFCu)|(vf&3));
}
uint64_t dirty(uint32_t slot){return uint64_t{1}<<(31-slot);}

void reached_and_accessor(){
    Fixture f;
    require(f.memory.load<uint32_t>(device+0x48C)==0x01000000&&f.memory.load<uint32_t>(device+0x490)==0&&
        f.memory.load<uint8_t>(device+0x2F44)==1&&f.memory.load<uint8_t>(device+0x2F92)==0,"original sampler defaults");
    auto min=f.guest.set_sampler_filter(device,0,sfr::SamplerFilter::minification,1);
    require(min==oracle(0x01000000,0,0,0,1,false)&&min.word3==0x01200000&&min.word4==2,"actual MIN linear");
    auto mag=f.guest.set_sampler_filter(device,0,sfr::SamplerFilter::magnification,1);
    require(mag==oracle(min.word3,min.word4,0,0,1,true)&&mag.word3==0x01280000&&mag.word4==3,"actual MAG linear");
    uint32_t inline_mip=(mag.word3&0xFE7FFFFF)|0x00800000;
    f.memory.store<uint32_t>(device+0x48C,inline_mip);
    require(f.guest.sampler_filter_state(0)==decode(inline_mip,mag.word4)&&inline_mip==0x00A80000,
        "accessor freshly reads direct inline MIP update");
}

void equations_and_preservation(){
    constexpr std::array<uint32_t,5> values{0,1,4,7,0xF1234567};
    for(bool mag:{false,true})for(uint32_t v:values){
        Fixture f; constexpr uint32_t slot=3,base=device+0x480+24*slot;
        uint32_t w3=0xA95ABCDE,w4=0xC3D2A5A5; f.memory.store<uint32_t>(base+12,w3);f.memory.store<uint32_t>(base+16,w4);
        f.memory.store<uint8_t>(device+0x2F44+slot,13);f.memory.store<uint8_t>(device+0x2F92+slot,mag?5:0xFD);
        f.memory.store<uint64_t>(device+24,0x8123456700000000ull);auto before=f.bytes();
        auto actual=f.guest.set_sampler_filter(device,slot,mag?sfr::SamplerFilter::magnification:sfr::SamplerFilter::minification,v);
        auto expected=oracle(w3,w4,5,mag?5:0xFD,v,mag);require(actual==expected,"independent raw filter equation");
        require(f.memory.load<uint32_t>(base+12)==expected.word3&&f.memory.load<uint32_t>(base+16)==expected.word4,
                "stored guest fetch words match the full raw-value oracle");
        for(size_t i=0;i<before.size();++i){bool changed=(i>=base-device+12&&i<base-device+20)||(i>=24&&i<32);
            if(!changed)require(f.memory.base()[device+i]==before[i],"unrelated sampler/device bytes preserved");}
        require(f.memory.load<uint64_t>(device+24)==(0x8123456700000000ull|dirty(slot)),"exact slot dirty bit");
    }
    Fixture last;last.memory.store<uint64_t>(device+24,0);last.guest.set_sampler_filter(device,25,sfr::SamplerFilter::minification,1);
    require(last.memory.load<uint64_t>(device+24)==0x40,"slot25 uses dirty bit6");
}

void guards_lookup_and_domain(){
    for(uint32_t relative:{0x490u,0x48Cu,0x18u}){Fixture f;auto before=f.bytes();
        uint32_t old=f.memory.load<uint32_t>(device+relative);f.memory.add_read_only_word(device+relative,[old]{return old;});
        rejects([&]{f.guest.set_sampler_filter(device,0,sfr::SamplerFilter::minification,1);},"late output guard rejects");
        require(f.bytes()==before,"late output guard is atomic");}
    Fixture reservation;auto before=reservation.bytes();uint32_t saved=reservation.memory.load_reserved_word(params);
    rejects([&]{reservation.guest.set_sampler_filter(device,0,sfr::SamplerFilter::magnification,1);},"reservation rejects");
    require(reservation.bytes()==before&&reservation.memory.store_conditional_word(params,saved),"reservation remains usable");
    Fixture provider;provider.memory.store<uint8_t>(device+0x2F44,16);
    provider.memory.add_read_only_word(lookup+64,[]{return 5u;});
    require(provider.guest.set_sampler_filter(device,0,sfr::SamplerFilter::minification,4)==oracle(0x01000000,0,5,0,4,false),
        "lookup word uses computed read provider");
    Fixture unknown;unknown.memory.store<uint8_t>(device+0x2F44,17);auto unchanged=unknown.bytes();
    rejects([&]{unknown.guest.set_sampler_filter(device,0,sfr::SamplerFilter::minification,1);},"N17 rejects");
    require(unknown.bytes()==unchanged,"unknown lookup has no effect");
    Fixture altered;altered.memory.store<uint32_t>(lookup+4,1);auto altered_before=altered.bytes();
    rejects([&]{altered.guest.set_sampler_filter(device,0,sfr::SamplerFilter::minification,1);},"altered known lookup rejects");
    require(altered.bytes()==altered_before,"unknown lookup value has no partial writes");
    rejects([&]{unknown.guest.set_sampler_filter(device+4,0,sfr::SamplerFilter::minification,1);},"wrong device rejects");
    rejects([&]{unknown.guest.set_sampler_filter(device,26,sfr::SamplerFilter::minification,1);},"slot26 rejects");
    rejects([&]{unknown.guest.set_sampler_filter(device,0,static_cast<sfr::SamplerFilter>(0x18),1);},"unknown filter rejects");
    rejects([&]{unknown.guest.sampler_filter_state(26);},"getter slot26 rejects");
}
}
int main(){try{reached_and_accessor();equations_and_preservation();guards_lookup_and_domain();
    std::cout<<"guest sampler filter tests passed\n";return 0;}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
