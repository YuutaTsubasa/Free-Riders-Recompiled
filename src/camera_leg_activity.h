#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <deque>
namespace sfr {
// Evidence of a deliberate leg sweep, used only to arbitrate the original kick.
class CameraLegActivity {
public:
    using Point=std::array<float,3>;
    struct Leg {Point hip,knee,ankle;};
    void update(const std::array<Leg,2>& legs,float dt,bool blocked=false) {
        if(blocked || !std::isfinite(dt) || dt<=0 || dt>.25f){reset();return;}
        Sample current;
        for(unsigned i=0;i<2;++i) {
            const auto& l=legs[i];
            for(const auto& p:{l.hip,l.knee,l.ankle})for(float v:p)
                if(!std::isfinite(v)){reset();return;}
            const auto length=[](const Point& a,const Point& b){return std::hypot(a[0]-b[0],a[1]-b[1],a[2]-b[2]);};
            const float upper=length(l.hip,l.knee),lower=length(l.knee,l.ankle);
            if(upper<.1f || upper>.85f || lower<.1f || lower>.85f){reset();return;}
            current.ankle[i]=l.ankle[2]-l.hip[2];current.knee[i]=l.knee[2]-l.hip[2];
        }
        hold_=std::max(0.f,hold_-dt);
        for(auto& s:history_)s.age+=dt;
        while(!history_.empty() && history_.front().age>.5f)history_.pop_front();
        if(!history_.empty()) {
            bool stable=true;
            for(unsigned i=0;i<2;++i)stable=stable &&
                std::abs(current.ankle[i]-history_.back().ankle[i])<.025f &&
                std::abs(current.knee[i]-history_.back().knee[i])<.025f;
            if(stable){current.stable=true;history_.back().stable=true;}
        }
        bool swept=false;Sample anchor;unsigned foot=0;
        for(const auto& old:history_)for(unsigned i=0;i<2;++i) {
            if(!old.stable)continue; // An isolated pose error cannot become the return-step anchor.
            const float a=current.ankle[i]-old.ankle[i],k=current.knee[i]-old.knee[i];
            if(std::abs(a)>=.08f && std::abs(k)>=.025f && a*k>0 &&
               std::abs(current.ankle[1-i]-old.ankle[1-i])<.05f &&
               std::abs(current.knee[1-i]-old.knee[1-i])<.05f){swept=true;anchor=old;foot=i;}
        }
        const float direction=current.ankle[foot]-anchor.ankle[foot];
        confirmed_=swept&&candidate_&&foot==candidate_foot_&&direction*candidate_direction_>0?confirmed_+dt:0;
        candidate_=swept;
        candidate_foot_=foot;candidate_direction_=direction;
        if(confirmed_>=.03f && !episode_) {
            anchor_=anchor;foot_=foot;direction_=current.ankle[foot]-anchor.ankle[foot];episode_=true;
            hold_=.65f;
        }
        // A confirmed forward step remains intentional while it is held.
        // Crossing back toward its starting position ends this extension;
        // the short remaining window lets the original detector finish.
        if(episode_ && (current.ankle[foot_]-anchor_.ankle[foot_])*direction_>0 &&
           std::abs(current.ankle[foot_]-anchor_.ankle[foot_])>=.06f &&
           (current.knee[foot_]-anchor_.knee[foot_])*direction_>0 &&
           std::abs(current.knee[foot_]-anchor_.knee[foot_])>=.02f &&
           std::abs(current.ankle[1-foot_]-anchor_.ankle[1-foot_])<.05f &&
           std::abs(current.knee[1-foot_]-anchor_.knee[1-foot_])<.05f)hold_=.65f;
        if(hold_==0)episode_=false;
        history_.push_back(current);
        if(history_.size()>64)history_.pop_front();
    }
    bool active() const {return hold_>0;}
private:
    struct Sample {std::array<float,2> ankle{},knee{};float age=0;bool stable=false;};
    void reset(){history_.clear();hold_=confirmed_=0;candidate_=episode_=false;}
    std::deque<Sample> history_;
    float hold_=0,confirmed_=0;
    Sample anchor_{};
    unsigned foot_=0;
    unsigned candidate_foot_=0;
    float candidate_direction_=0;
    float direction_=0;
    bool candidate_=false,episode_=false;
};
}
