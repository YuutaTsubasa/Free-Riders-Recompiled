#pragma once
#include <array>
#include <algorithm>
#include <cmath>
namespace sfr {
// Camera-only gesture arbitration, measured between fresh pose observations.
class CameraArmActivity {
public:
    using Point=std::array<float,3>;
    struct Arm {Point shoulder,elbow,wrist;};
    void update(const std::array<Arm,2>& arms,float dt) {
        hold_=std::max(0.f,hold_-dt);
        for(unsigned i=0;i<2;++i) {
            const auto& a=arms[i];
            const auto distance=[](const Point& a,const Point& b) {
                return std::hypot(a[0]-b[0],a[1]-b[1],a[2]-b[2]);
            };
            const float upper=distance(a.shoulder,a.elbow),lower=distance(a.elbow,a.wrist);
            bool valid=upper>.08f&&upper<.75f&&lower>.08f&&lower<.75f;
            for(const auto& p:{a.shoulder,a.elbow,a.wrist})for(float v:p)valid=valid&&std::isfinite(v);
            if(!valid){seen_[i]=false;continue;}
            Point relative{};
            for(unsigned axis=0;axis<3;++axis)relative[axis]=a.wrist[axis]-a.shoulder[axis];
            const float movement=seen_[i]?distance(relative,previous_[i]):0;
            const bool raised=a.wrist[1]>a.shoulder[1]+.05f;
            if(raised||(seen_[i]&&movement>.01f&&movement/dt>.8f))hold_=.3f;
            previous_[i]=relative;seen_[i]=true;
        }
    }
    bool active() const {return hold_>0;}
private:
    std::array<Point,2> previous_{};
    std::array<bool,2> seen_{};
    float hold_=0;
};
}
