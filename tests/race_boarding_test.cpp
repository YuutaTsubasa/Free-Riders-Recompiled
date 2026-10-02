#include "race_boarding.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>

struct Memory {
    std::array<unsigned char, 4096> bytes{};
    template<class T> T load(uint64_t p) const { T v{}; std::memcpy(&v, bytes.data()+p, sizeof(v)); return v; }
    template<class T> void store(uint64_t p, T v) { std::memcpy(bytes.data()+p, &v, sizeof(v)); }
    void f(uint32_t p, float v) { store(p, std::bit_cast<uint32_t>(v)); }
    float f(uint32_t p) const { return std::bit_cast<float>(load<uint32_t>(p)); }
};
constexpr uint32_t object=128, first=1024, second=2048;
constexpr float rate=1.2268518209457397f, target_rate=0.6481481481481481f;
void require(bool b, const char* why) { if(!b) { std::cerr << why << '\n'; std::exit(1); } }
void near(float a,float b) { require(std::abs(a-b)<0.0001f,"unexpected boarding displacement"); }
Memory fixture(float distance=20) {
    Memory m;
    m.store<uint32_t>(object+336,1); m.store<uint32_t>(object+340,1);
    m.store<uint8_t>(object+500,1);
    m.f(object+32,distance); m.f(object+44,1);
    for(auto r:{first,second}) { m.f(r+368,rate); m.f(r+380,1); m.f(r+300,1); }
    m.f(object+736,0.75f);
    return m;
}
void scales_and_preserves_state() {
    for(float frames:{0.25f,0.5f,1.0f,2.0f,3.0f,8.0f}) {
        auto m=fixture(); auto before=m;
        sfr::correct_boarding_step(m,object,first,second,frames,1);
        near(m.f(first+368),rate*frames); near(m.f(second+368),rate*frames);
        near(m.f(first+288),0); near(m.f(object+736),0.75f);
        require(m.load<uint32_t>(object+340)==1,"early transition");
        if(frames==1) require(m.bytes==before.bytes,"one-frame behavior changed");
    }
}
void reach_matches_original_pair_snap() {
    for(float frames:{2.0f,3.0f,1000000.0f}) {
        auto m=fixture(2); m.f(second+288,-7);
        sfr::correct_boarding_step(m,object,first,second,frames,1);
        for(auto r:{first,second}) for(uint32_t field:{288u,368u}) {
            near(m.f(r+field),2); near(m.f(r+field+12),1);
        }
        require(m.load<uint32_t>(object+340)==2 && m.load<uint8_t>(object+349)==1,"missing boarding transition");
        require(m.load<uint32_t>(object+336)==1,"caller must perform current-state transition");
    }
    auto m=fixture(2); sfr::correct_boarding_step(m,object,first,first,2,1);
    near(m.f(first+368),2);
}
void unchanged_other_states_and_invalid_frames() {
    for(int variation=0;variation<9;++variation) {
        auto m=fixture(); float frames=2; uint8_t phase=1;
        if(variation==0) phase=0; // phase 0 can become 1 during the original update
        if(variation==1) m.store<uint32_t>(object+336,2);
        if(variation==2) m.store<uint32_t>(object+340,2);
        if(variation==3) m.store<uint8_t>(object+349,1);
        if(variation==4) frames=0;
        if(variation==5) frames=-1;
        if(variation==6) frames=std::numeric_limits<float>::infinity();
        if(variation==7) frames=std::numeric_limits<float>::quiet_NaN();
        if(variation==8) m.store<uint8_t>(object+500,2);
        auto before=m; sfr::correct_boarding_step(m,object,first,second,frames,phase);
        require(m.bytes==before.bytes,"changed an ineligible update");
    }
}
void blended_direction_and_pair_offsets() {
    auto m=fixture(100);
    m.f(first+288,10); m.f(first+292,20); m.f(first+296,30);
    m.f(first+368,10.3f); m.f(first+372,20.4f); m.f(first+376,30);
    m.f(second+288,40); m.f(second+292,50); m.f(second+296,60);
    sfr::correct_boarding_step(m,object,first,second,3,1);
    near(m.f(first+368),10.9f); near(m.f(first+372),21.2f);
    near(m.f(second+368),40.9f); near(m.f(second+372),51.2f);
    near(m.f(second+376),60); near(m.f(second+288),40);
    near(m.f(object+736),0.75f);
    // Preserve the blend magnitude rather than normalizing it into a faster step.
    near(std::hypot(m.f(first+368)-10,m.f(first+372)-20),1.5f);
}
void moving_target_regression() {
    // Captured Frozen Forest rates: unscaled boarding cannot catch the target at dt=2.
    require(rate < 2*target_rate,"regression no longer demonstrates the original speed mismatch");
    for(float frames:{0.5f,1.0f,2.0f,3.0f,8.0f}) {
        auto m=fixture(); float target=20, rider=0; bool boarded=false;
        for(int i=0;i<200;++i) {
            target+=target_rate*frames; m.f(object+32,target); m.f(first+288,rider);
            m.f(first+368,rider+rate);
            // The original reaches within one unit before applying its emitted step.
            if(target-rider<1) { boarded=true; break; }
            sfr::correct_boarding_step(m,object,first,first,frames,1);
            if(m.load<uint8_t>(object+349)) { boarded=true; break; }
            rider=m.f(first+368);
            require(rider<=target,"overshot moving target");
        }
        require(boarded,"boarding failed to catch a continuously moving path target");
    }
}
int main() {
    scales_and_preserves_state(); reach_matches_original_pair_snap();
    unchanged_other_states_and_invalid_frames(); blended_direction_and_pair_offsets(); moving_target_regression();
    std::cout << "race boarding tests passed\n";
}
