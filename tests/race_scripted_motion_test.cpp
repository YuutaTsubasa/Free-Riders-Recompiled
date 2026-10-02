#include "race_scripted_motion.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

struct Memory {
    std::array<unsigned char,4096> bytes{};
    static uint64_t offset(uint64_t p) {return p>=0x821848B8 && p<0x82184900?p-0x821848B8+16:p;}
    template<class T> T load(uint64_t p) const { T v;std::memcpy(&v,bytes.data()+offset(p),sizeof(v));return v; }
    template<class T> void store(uint64_t p,T v) {std::memcpy(bytes.data()+offset(p),&v,sizeof(v));}
    void f(uint32_t p,float v) {store(p,std::bit_cast<uint32_t>(v));}
    float f(uint32_t p) const {return std::bit_cast<float>(load<uint32_t>(p));}
};
constexpr uint32_t physics=128;
void require(bool b,const char* why) {if(!b){std::cerr<<why<<'\n';std::exit(1);}}
void near(float a,float b) {require(std::abs(a-b)<0.002f,"scripted displacement uses updates instead of elapsed time");}
Memory fixture() {
    Memory m;m.store<uint32_t>(physics+2392,1);
    m.f(physics+256,-2);m.f(physics+260,6.944444f);m.f(physics+264,0.25f);
    m.f(physics+268,1);return m;
}
void elapsed_time_regression() {
    for(float step:{0.5f,1.0f,2.0f,3.0f,6.0f}) {
        auto m=fixture();auto before=m.bytes;float x=0,y=0,z=0;int calls=0;
        for(float elapsed=0;elapsed<60;elapsed+=step) {
            sfr::with_scripted_force_step(m,physics,step,[&]{
                x+=m.f(physics+256);y+=m.f(physics+260);z+=m.f(physics+264);++calls;
            });
            require(m.bytes==before,"temporary displacement compounded or leaked");
        }
        near(x,-120);near(y,6.944444f*60);near(z,15);
        require(calls==int(60/step),"original update called more than once");
    }
}
void preserve_original_changes_and_unwind() {
    auto m=fixture();
    sfr::with_scripted_force_step(m,physics,2,[&]{m.f(physics+256,17);});
    near(m.f(physics+256),17);near(m.f(physics+260),6.944444f);near(m.f(physics+264),0.25f);
    m=fixture();auto before=m.bytes;bool caught=false;
    try {sfr::with_scripted_force_step(m,physics,2,[&]{throw std::runtime_error("guest failure");});}
    catch(const std::runtime_error&) {caught=true;}
    require(caught && m.bytes==before,"exception left scaled input in guest memory");
}
void unaffected_modes_and_invalid_steps() {
    for(uint32_t mode:{0u,2u,3u}) {
        auto m=fixture();m.store<uint32_t>(physics+2392,mode);auto before=m.bytes;
        sfr::with_scripted_force_step(m,physics,3,[&]{require(m.bytes==before,"another motion mode changed");});
    }
    for(float step:{1.0f,0.0f,-1.0f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        auto m=fixture();auto before=m.bytes;
        sfr::with_scripted_force_step(m,physics,step,[&]{require(m.bytes==before,"identity/invalid step changed input");});
    }
    for(float value:{std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::max()}) {
        auto m=fixture();m.f(physics+260,value);auto before=m.bytes;
        sfr::with_scripted_force_step(m,physics,2,[&]{require(m.bytes==before,"nonfinite or overflowing vector was partly scaled");});
        require(m.bytes==before,"invalid vector changed after update");
    }
}
void skipped_path_boundary() {
    constexpr uint32_t path=1024;
    for(uint32_t route:{0u,1u}) for(uint16_t node:{uint16_t(132),uint16_t(133),uint16_t(134),uint16_t(143),uint16_t(144),uint16_t(149)}) {
        Memory m;const uint32_t boundary=route?144:133;
        m.store<uint32_t>(path,0x123400);m.store<uint32_t>(path+228,route);
        m.store<uint16_t>(path+90,node);m.store<uint8_t>(path+248,1);
        m.store<uint32_t>(0x821848B8+route*36+32,boundary);
        int calls=0;
        sfr::with_scripted_path_update(m,path,[&]{
            ++calls;m.store<uint16_t>(path+90,node+5);
            // The original only changes phase on an exact old-node match.
            if(node==boundary)m.store<uint8_t>(path+248,0);
        });
        require(calls==1,"path update was replayed");
        require(m.load<uint8_t>(path+248)==(node<boundary?1:0),"skipping the exact path boundary left deceleration active");
    }
    for(int change=0;change<5;++change) {
        Memory m;m.store<uint32_t>(path,0x123400);m.store<uint32_t>(path+228,1);
        m.store<uint16_t>(path+90,149);m.store<uint8_t>(path+248,1);
        m.store<uint32_t>(0x821848B8+36+32,144);
        if(change==4)m.store<uint32_t>(path+228,2);
        sfr::with_scripted_path_update(m,path,[&]{
            if(change==0)m.store<uint32_t>(path,0);
            if(change==1)m.store<uint32_t>(path,0x567800);
            if(change==2)m.store<uint32_t>(path+228,0);
            if(change==3)m.store<uint16_t>(path+90,0);
        });
        require(m.load<uint8_t>(path+248)==1,"completed or retargeted path changed");
    }
}
int main(int argc,char**) {
    if(argc>1){skipped_path_boundary();return 0;}
    elapsed_time_regression();preserve_original_changes_and_unwind();unaffected_modes_and_invalid_steps();
    skipped_path_boundary();
    std::cout<<"scripted motion tests passed\n";
}
