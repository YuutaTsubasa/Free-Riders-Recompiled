#include "camera_race_motion.h"
#include "camera_leg_activity.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
sfr::CameraRacePose pose(float lean=0,float pitch=.2f,float height=.8f) {
    return {{0,0,-2.5f},{-.5f*std::tan(lean),.5f,-2.5f+.5f*std::tan(pitch)},
            {-.1f,-height,-2.5f},{.1f,-height,-2.5f}};
}
void frames(sfr::CameraRaceMotion& motion,const sfr::CameraRacePose& p,int count) {
    for(int i=0;i<count;++i) motion.update(p,1.f/60);
}
int main() {
    try {
        using Leg=sfr::CameraLegActivity::Leg;
        const std::array<Leg,2> planted{{{{-.12f,0,-2.5f},{-.12f,-.4f,-2.5f},{-.12f,-.8f,-2.5f}},
                                       {{.12f,0,-2.5f},{.12f,-.4f,-2.5f},{.12f,-.8f,-2.5f}}}};
        for(unsigned foot:{0u,1u}) {
            sfr::CameraLegActivity leg;
            for(int i=0;i<20;++i)leg.update(planted,.05f);
            require(!leg.active(),"standing legs cannot bypass hand protection");
            auto sweep=planted;sweep[foot].ankle[2]+=.12f;sweep[foot].knee[2]+=.06f;
            leg.update(sweep,.05f);
            require(!leg.active(),"one coherent pose outlier needs fresh confirmation");
            leg.update(sweep,.05f);
            require(leg.active(),"coherent ankle and knee sweep allows kick despite balancing hands");
            for(int i=0;i<30;++i)leg.update(sweep,.05f);
            require(leg.active(),"holding a confirmed forward foot must not lose Ready to balancing hands");
            leg.update(planted,.05f);leg.update(planted,.05f);require(leg.active(),"returning the swept foot has fresh leg evidence");
            for(int i=0;i<30;++i)leg.update(planted,.05f);
            require(!leg.active(),"returning to neutral restores hand protection after the bounded release window");
            leg.update(planted,.3f);require(!leg.active(),"camera stall clears leg evidence");
            leg.update(sweep,.05f);require(!leg.active(),"reacquisition alone cannot infer a swept foot");
            leg.update(planted,.05f,true);require(!leg.active(),"jump protection clears leg evidence");
            sfr::CameraLegActivity noisy;noisy.update(planted,.05f);
            auto ankle_only=planted;ankle_only[foot].ankle[2]+=.2f;
            noisy.update(ankle_only,.05f);require(!noisy.active(),"one ankle depth outlier is not a deliberate kick");
            sfr::CameraLegActivity outlier;
            for(int i=0;i<20;++i)outlier.update(planted,.05f);
            outlier.update(sweep,.05f);
            for(int i=0;i<40;++i){outlier.update(planted,.05f);
                require(!outlier.active(),"one coherent outlier cannot make neutral stance a latched kick");}
            sfr::CameraLegActivity both;both.update(planted,.05f);
            auto shifted=planted;for(auto& l:shifted){l.knee[2]+=.1f;l.ankle[2]+=.2f;}
            both.update(shifted,.05f);require(!both.active(),"both feet shifting cannot bypass the hand guard");
        }
        sfr::CameraRaceMotion m;
        frames(m,pose(),60);
        require(m.ready() && m.lean()==0 && !m.boost() && !m.crouch() && !m.jump(),"standing calibrates without input");
        frames(m,pose(-.45f),30);require(m.lean()<-.5f,"left tilt steers left");
        frames(m,pose(.45f),30);require(m.lean()>.5f,"right tilt steers right");
        frames(m,pose(),30);require(m.lean()==0,"upright returns to neutral");
        frames(m,pose(0,.35f),60);require(!m.boost(),"small model pitch variation does not Boost");
        frames(m,pose(0,.65f),4);require(!m.boost(),"short forward noise does not Boost");
        frames(m,pose(0,.65f),30);require(m.boost(),"deliberate sustained forward tilt Boosts");
        frames(m,pose(),30);require(!m.boost(),"upright releases Boost");
        frames(m,pose(0,.65f,.5f),30);require(m.crouch() && !m.boost(),"crouch arms jump and excludes Boost");
        bool jumped=false;int pulses=0;
        for(int i=0;i<30;++i) {m.update(pose(),1.f/60);if(m.jump()){jumped=true;++pulses;}}
        require(jumped && pulses==1 && !m.crouch(),"standing from crouch emits one jump");
        frames(m,pose(0,.2f,.5f),30);m.reset();frames(m,pose(),60);
        require(m.ready() && !m.jump(),"source loss does not release a jump");
        auto bad=pose();bad.hip[1]=NAN;m.update(bad,1.f/60);
        require(!m.ready() && m.lean()==0 && !m.boost() && !m.jump(),"invalid body resets safely");
        frames(m,pose(),60);m.update(pose(),1.f);
        require(!m.ready(),"stalled samples reset calibration");
        sfr::CameraRaceMotion moving;
        for(int i=0;i<120;++i)moving.update(pose(i%2?.5f:-.5f),1.f/60);
        require(!moving.ready(),"moving pose is not a neutral calibration");
        sfr::CameraRaceMotion settling;
        frames(settling,pose(0,.2f,.6f),60);
        frames(settling,pose(0,.2f,.8f),300);
        frames(settling,pose(0,.2f,.52f),40);
        require(settling.crouch(),"standing leg estimate settling after entry must not hide later crouches");
        sfr::CameraRaceMotion stalled;
        for(uint64_t n=1;n<=60;++n)stalled.observe(pose(),1.f/60,n);
        for(int i=0;i<24;++i) {
            stalled.observe(pose(0,.8f),1.f/60,61);
            require(!stalled.boost() && !stalled.crouch() && !stalled.jump(),"a reused outlier must never activate even briefly");
        }
        require(!stalled.boost() && !stalled.crouch() && !stalled.jump(),"one camera outlier reused during a stall cannot qualify a gesture");
        sfr::CameraRaceMotion slow;
        for(uint64_t n=1;n<=60;++n)slow.observe(pose(),1.f/60,n);
        for(int i=0;i<12;++i)slow.observe(pose(),1.f/60,60);
        slow.observe(pose(0,.8f),1.f/60,61);
        require(!slow.boost(),"first forward sample cannot borrow preceding neutral time");
        for(uint64_t n=62;n<100;++n)slow.observe(pose(),1.f/60,n);
        for(int i=0;i<10;++i)slow.observe(pose(),1.f/60,99);
        slow.observe(pose(0,.2f,.4f),1.f/60,100);
        require(!slow.crouch(),"first crouched sample cannot borrow preceding standing time");
        sfr::CameraRaceMotion rising;
        frames(rising,pose(),60);frames(rising,pose(0,.2f,.4f),30);
        int rises=0;
        for(int i=0;i<18;++i) {rising.update(pose(0,.2f,.6f),1.f/60);if(rising.jump())++rises;}
        require(rises==1,"deep crouch recovery should jump before returning to full standing height");
        frames(rising,pose(),30);require(!rising.jump(),"finishing the same rise must not jump again");
        auto front=pose();
        front.left_shoulder={.18f,.5f,-2.5f};front.right_shoulder={-.18f,.5f,-2.5f};
        front.left_hip={.125f,0,-2.5f};front.right_hip={-.125f,0,-2.5f};
        auto side_pose=front;
        side_pose.left_shoulder={.02f,.5f,-2.32f};side_pose.right_shoulder={-.02f,.5f,-2.68f};
        side_pose.left_hip={.02f,0,-2.375f};side_pose.right_hip={-.02f,0,-2.625f};
        sfr::CameraRaceMotion facing;
        frames(facing,side_pose,60);require(!facing.brake_ready(),"sideways riding cannot arm brake");
        frames(facing,front,5);require(!facing.brake_ready(),"brief front-facing jitter cannot arm brake");
        frames(facing,front,30);require(facing.brake_ready(),"sustained turn to face sensor allows original brake");
        auto disagree=front;disagree.left_hip=side_pose.left_hip;disagree.right_hip=side_pose.right_hip;
        frames(facing,disagree,1);require(!facing.brake_ready(),"shoulders and pelvis must agree on facing sensor");
        frames(facing,front,30);front.left_ankle[1]=front.right_ankle[1]=-.4f;
        frames(facing,front,30);require(facing.crouch()&&!facing.brake_ready(),"crouch suppresses frontal brake");
        front.left_ankle[1]=front.right_ankle[1]=-.6f;
        frames(facing,front,18);require(!facing.brake_ready(),"early recovery cannot be cancelled by brake or Side");
        front.left_ankle[1]=front.right_ankle[1]=-.8f;
        frames(facing,front,60);require(facing.brake_ready(),"normal brake returns after recovery");
        sfr::CameraRaceMotion knee_outlier;
        for(int i=0;i<10;++i)knee_outlier.update(pose(),.1f);
        for(int i=0;i<10;++i)knee_outlier.update(pose(0,.2f,.5f),.1f);
        knee_outlier.update(pose(0,.2f,.3f),.1f);
        for(int i=0;i<10;++i){knee_outlier.update(pose(0,.2f,.5f),.1f);
            require(!knee_outlier.jump(),"one low leg estimate followed by the same crouch cannot jump");}
        sfr::CameraRaceMotion high_outlier;
        for(int i=0;i<10;++i)high_outlier.update(pose(),.1f);
        for(int i=0;i<10;++i)high_outlier.update(pose(0,.2f,.5f),.1f);
        high_outlier.update(pose(0,.2f,1.f),.1f);
        require(!high_outlier.jump(),"one high leg estimate cannot jump immediately");
        for(int i=0;i<10;++i){high_outlier.update(pose(0,.2f,.5f),.1f);
            require(!high_outlier.jump(),"filtered high outlier cannot count as two rising raw poses");}
        auto arm_pose=front;arm_pose.shoulder=pose().shoulder;
        arm_pose.left_elbow={.25f,.28f,-2.5f};arm_pose.left_wrist={.25f,.1f,-2.4f};
        arm_pose.right_elbow={-.25f,.28f,-2.5f};arm_pose.right_wrist={-.25f,.1f,-2.4f};
        sfr::CameraRaceMotion waving;frames(waving,arm_pose,60);
        for(int i=0;i<45;++i) {
            auto wave=arm_pose;wave.shoulder=pose(0,.7f).shoulder;
            wave.right_wrist={-.25f+.18f*std::sin(i*.4f),.35f,-2.3f};
            waving.update(wave,1.f/60);
            require(!waving.boost() && !waving.brake_ready(),"waving takes priority over body Boost and brake");
        }
        auto lingering=arm_pose;lingering.shoulder=pose(0,.7f).shoulder;
        for(int i=0;i<90;++i){waving.update(lingering,1.f/60);
            require(!waving.boost()&&!waving.brake_ready(),"throw follow-through cannot become delayed Boost or brake");}
        frames(waving,arm_pose,60); // Neutral pitch, still frontal: brake needs riding stance first.
        require(!waving.boost()&&!waving.brake_ready(),"upright alone cannot turn a throw into a frontal brake");
        frames(waving,lingering,60);require(waving.boost(),"new deliberate forward tilt works after returning neutral");
        auto riding=side_pose;
        riding.left_elbow={.1f,.28f,-2.32f};riding.left_wrist={.1f,.1f,-2.22f};
        riding.right_elbow={-.1f,.28f,-2.68f};riding.right_wrist={-.1f,.1f,-2.58f};
        frames(waving,riding,60);frames(waving,arm_pose,90);
        require(waving.brake_ready(),"deliberate frontal brake works after returning to riding stance");
        auto up=arm_pose;up.right_elbow={-.25f,.55f,-2.5f};up.right_wrist={-.25f,.8f,-2.4f};
        frames(waving,up,60);require(waving.arm_action()&&!waving.brake_ready(),"held throwing prep remains protected");
        frames(waving,arm_pose,60);require(!waving.brake_ready(),"throw ends frontal but remains disarmed");
        auto false_side=arm_pose;false_side.left_hip=riding.left_hip;false_side.right_hip=riding.right_hip;
        waving.update(false_side,1.f/60);frames(waving,arm_pose,45);
        require(!waving.brake_ready(),"one pelvis yaw outlier must not rearm frontal braking after a throw");
        sfr::CameraRaceMotion menu_hand;
        frames(menu_hand,up,60);require(!menu_hand.ready(),"raised menu hand must not become a neutral race calibration");
        frames(menu_hand,riding,120);require(menu_hand.ready(),"lowering menu hand allows normal race calibration");
        sfr::CameraRaceMotion torso_translation;frames(torso_translation,arm_pose,60);
        for(int i=0;i<30;++i){auto translated=arm_pose;
            for(auto* point:{&translated.hip,&translated.shoulder,&translated.left_ankle,&translated.right_ankle,
                &translated.left_shoulder,&translated.right_shoulder,&translated.left_hip,&translated.right_hip,
                &translated.left_elbow,&translated.left_wrist,&translated.right_elbow,&translated.right_wrist})
                (*point)[0]+=i*.05f;
            torso_translation.update(translated,1.f/60);
            require(!torso_translation.arm_action(),"moving the whole body is not a shoulder-relative arm swing");}
        waving.reset();frames(waving,riding,60);require(!waving.arm_action(),"source reset clears arm activity");
        sfr::CameraRaceMotion stalled_arm;
        for(uint64_t n=1;n<=90;++n)stalled_arm.observe(arm_pose,1.f/60,n);
        stalled_arm.observe(up,1.f/60,91);require(stalled_arm.arm_action(),"fresh raised hand starts protection");
        for(int i=0;i<30;++i)stalled_arm.observe(up,1.f/60,91);
        require(!stalled_arm.ready()&&!stalled_arm.arm_action()&&!stalled_arm.boost()&&!stalled_arm.brake_ready(),
                "lost camera clears hand protection and all control qualification safely");
        for(uint64_t n=92;n<=220;++n)stalled_arm.observe(riding,1.f/60,n);
        require(stalled_arm.ready()&&!stalled_arm.arm_action(),"fresh riding pose recovers after camera loss");
        sfr::CameraRaceMotion throwing;
        frames(throwing,arm_pose,60);
        auto prep=arm_pose;prep.right_elbow={-.25f,.38f,-2.5f};prep.right_wrist={-.25f,.6f,-2.22f};
        frames(throwing,prep,6);require(!throwing.overthrow(),"raised throwing prep alone cannot throw");
        auto release=prep;release.right_wrist={-.25f,.38f,-2.1f};
        throwing.update(release,1.f/60);
        require(throwing.overthrow(),"camera diagonal raised forearm followed by forward swing must over-throw");
        throwing.update(release,1.f/60);require(!throwing.overthrow(),"one arm swing cannot throw repeatedly");
        throwing.reset();throwing.update(release,.1f);
        require(!throwing.overthrow(),"forward movement without preparation cannot throw");
        throwing.update(prep,.1f);frames(throwing,arm_pose,40);throwing.update(release,.1f);
        require(!throwing.overthrow(),"abandoned preparation expires");
        throwing.reset();throwing.observe(prep,.1f,1);
        for(int i=0;i<8;++i)throwing.observe(prep,.1f,1);
        throwing.observe(release,.1f,2);
        require(!throwing.overthrow(),"cached preparation and lost tracking cannot produce a throw");
        throwing.update(prep,.1f);throwing.reset();throwing.update(release,.1f);
        require(!throwing.overthrow(),"source reset clears prepared throw");
        for(float invalid:{10.f,std::numeric_limits<float>::quiet_NaN()}) {
            auto bad_prep=prep,bad_release=release;
            bad_prep.right_shoulder[0]=bad_release.right_shoulder[0]=invalid;
            throwing.reset();throwing.update(bad_prep,.1f);throwing.update(bad_release,.1f);
            require(!throwing.overthrow(),"invalid shoulder or upper arm cannot prepare a throw");
        }
        std::cout<<"camera race motion tests passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
