#pragma once
#include "camera_arm_activity.h"
#include <array>
#include <cmath>
namespace sfr {
// Same raised-forearm then forward-swing gesture as the original OverThrow.
// Only fresh camera observations enter update; no guest frame-history writes.
class CameraOverthrow {
public:
    using Point=CameraArmActivity::Point;
    using Arm=CameraArmActivity::Arm;
    void clear_pulse(){fired_=false;}
    void reset(){*this=CameraOverthrow{};}
    void update(const Point& shoulder_center,const std::array<Arm,2>& arms,float dt) {
        fired_=false;
        for(unsigned i=0;i<2;++i) {
            const auto& a=arms[i];
            auto& s=state_[i];
            bool valid=std::isfinite(dt)&&dt>0&&dt<=.25f;
            for(const auto& p:{shoulder_center,a.shoulder,a.elbow,a.wrist})for(float v:p)valid=valid&&std::isfinite(v);
            const float x=a.wrist[0]-a.elbow[0],y=a.wrist[1]-a.elbow[1],z=a.wrist[2]-a.elbow[2];
            const float length=std::hypot(x,y,z);
            const float upper=std::hypot(a.elbow[0]-a.shoulder[0],a.elbow[1]-a.shoulder[1],a.elbow[2]-a.shoulder[2]);
            if(!valid||length<=.08f||length>=.75f||upper<=.08f||upper>=.75f){s={};continue;}
            s.age+=dt;
            if(s.age>.5f)s.armed=false;
            const float movement=s.seen?std::hypot(a.wrist[0]-s.previous[0],a.wrist[1]-s.previous[1],a.wrist[2]-s.previous[2]):0;
            if(s.armed && z/length>.8f && movement>.01f) {
                fired_=true;s.armed=false;
            } else if(a.wrist[1]>shoulder_center[1] && y/length>.6f) {
                // A fresh observed held prep is valid; reusing a cached frame
                // never calls this or extends its 0.5 second release window.
                s.armed=true;s.age=0;
            }
            s.previous=a.wrist;s.seen=true;
        }
    }
    bool fired() const{return fired_;}
private:
    struct State {Point previous{};float age=0;bool armed=false,seen=false;};
    std::array<State,2> state_{};
    bool fired_=false;
};
}
