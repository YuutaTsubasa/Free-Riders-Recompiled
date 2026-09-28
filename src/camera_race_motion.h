#pragma once
#include <array>
#include <optional>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "camera_arm_activity.h"
#include "camera_overthrow.h"
#include "camera_leg_activity.h"
namespace sfr {
// Guest body coordinates: +Y is up, +Z is toward the player-facing sensor,
// and +X is opposite the menu skeleton's/player's right.
struct CameraRacePose {
    std::array<float,3> hip,shoulder,left_ankle,right_ankle;
    std::array<float,3> left_shoulder{},right_shoulder{},left_hip{},right_hip{};
    std::array<float,3> left_elbow{},left_wrist{},right_elbow{},right_wrist{};
    std::array<float,3> left_knee{},right_knee{};
    // When set, the height of the hips is taken above this floor instead of
    // the lower ankle: a real Kinect placed high rarely sees the feet, and
    // its guessed ankles made crouches come and go.
    std::optional<float> floor_y{};
};
class CameraRaceMotion {
public:
    // Called at game cadence, but qualification advances at camera cadence.
    // A held/repeated sample is not evidence of a sustained gesture.
    void observe(const CameraRacePose& p,float dt,uint64_t sequence) {
        jump_=false;overthrow_.clear_pulse();
        if(!sequence || !std::isfinite(dt) || dt<=0 || dt>.25f){reset();return;}
        elapsed_+=dt;
        if(sequence==sequence_) {
            if(elapsed_>.25f) {
                reset();sequence_=sequence;elapsed_=.3f;
            }
            return;
        }
        const float interval=elapsed_;
        elapsed_=0;
        update(p,interval);
        sequence_=sequence;
    }
    void update(const CameraRacePose& p,float dt) {
        jump_=false;overthrow_.clear_pulse();
        for(const auto& point:{p.hip,p.shoulder,p.left_ankle,p.right_ankle})
            for(float v:point) if(!std::isfinite(v)){reset();return;}
        const float y=p.shoulder[1]-p.hip[1];
        const float height=p.hip[1]-(p.floor_y?*p.floor_y:std::min(p.left_ankle[1],p.right_ankle[1]));
        if(!std::isfinite(dt)||dt<=0||dt>.25f||y<.15f||y>1.2f||(!p.floor_y&&(height<.25f||height>1.8f))){reset();return;}
        arms_.update({CameraArmActivity::Arm{p.left_shoulder,p.left_elbow,p.left_wrist},
                      CameraArmActivity::Arm{p.right_shoulder,p.right_elbow,p.right_wrist}},dt);
        overthrow_.update(p.shoulder,{CameraOverthrow::Arm{p.left_shoulder,p.left_elbow,p.left_wrist},
                                      CameraOverthrow::Arm{p.right_shoulder,p.right_elbow,p.right_wrist}},dt);
        if(arms_.active()) {
            boost_neutral_required_=brake_side_required_=true;
            neutral_candidate_=side_candidate_=false;neutral_time_=side_time_=0;
        }
        const float roll=std::atan2(p.hip[0]-p.shoulder[0],y);
        const float pitch=std::atan2(p.shoulder[2]-p.hip[2],y);
        if(!ready_) {
            if(arms_.active()){calibration_=0;return;}
            if(calibration_==0 || std::abs(roll-anchor_roll_)>.0873f ||
               std::abs(pitch-anchor_pitch_)>.0873f || std::abs(height-anchor_height_)>anchor_height_*.15f) {
                anchor_roll_=roll;anchor_pitch_=pitch;anchor_height_=height;
                roll_sum_=pitch_sum_=height_sum_=calibration_=0;
            }
            calibration_+=dt;roll_sum_+=roll*dt;pitch_sum_+=pitch*dt;height_sum_+=height*dt;
            if(calibration_<.6f)return;
            neutral_roll_=roll_sum_/calibration_;neutral_pitch_=pitch_sum_/calibration_;
            neutral_height_=height_sum_/calibration_;
            filtered_roll_=neutral_roll_;filtered_pitch_=neutral_pitch_;filtered_height_=neutral_height_;
            ready_=true;
        }
        const float a=1-std::exp(-dt/.08f);
        filtered_roll_+=a*(roll-filtered_roll_);filtered_pitch_+=a*(pitch-filtered_pitch_);
        filtered_height_+=a*(height-filtered_height_);
        const float side=filtered_roll_-neutral_roll_;
        // The model's legs can settle longer than the torso at race entry.
        // Learn a taller stable upright pose slowly, never a crouched baseline.
        if(!crouch_ && std::abs(side)<.174533f && std::abs(filtered_pitch_-neutral_pitch_)<.174533f &&
           filtered_height_>neutral_height_)
            neutral_height_+=(1-std::exp(-dt/2.f))*(filtered_height_-neutral_height_);
        lean_=std::copysign(std::clamp((std::abs(side)-.0698132f)/(.523599f-.0698132f),0.f,1.f),side);
        if(lean_==0)lean_=0; // keep neutral positive zero for guest sign-bit consumers
        const bool compressed=filtered_height_<neutral_height_*.8f;
        if(jump_recovering_ && filtered_height_>neutral_height_*.9f)jump_recovering_=false;
        if(!crouch_ && !jump_recovering_) {
            // Only the interval between two qualifying observations counts.
            crouch_time_=compressed&&crouch_candidate_?crouch_time_+dt:0;
            crouch_candidate_=compressed;
            if(crouch_time_>=.12f){crouch_=true;lowest_height_=std::max({height,previous_height_,filtered_height_});}
        } else if(crouch_) {
            // A lone low leg estimate followed by the same crouch is not a
            // takeoff. Confirm a lower baseline with two fresh raw samples.
            lowest_height_=std::min(lowest_height_,std::max({height,previous_height_,filtered_height_}));
            // Trigger on a clear rise from the actual crouch, not only after
            // returning almost to full standing height (too late for a ramp).
            const float rise_threshold=std::min(neutral_height_*.9f,
                lowest_height_+std::max(.06f,neutral_height_*.12f));
            const bool rising=height>rise_threshold && filtered_height_>rise_threshold;
            rise_time_=rising&&rise_candidate_?rise_time_+dt:0;
            rise_candidate_=rising;
            if(rise_time_>=.04f) {
                crouch_=false;crouch_candidate_=false;crouch_time_=0;
                rise_candidate_=false;rise_time_=0;jump_=true;recovery_=.25f;jump_recovering_=true;
            }
        }
        previous_height_=height;
        recovery_=std::max(0.f,recovery_-dt);
        legs_.update({CameraLegActivity::Leg{p.left_hip,p.left_knee,p.left_ankle},
                      CameraLegActivity::Leg{p.right_hip,p.right_knee,p.right_ankle}},dt,jump_protected());
        const float forward=filtered_pitch_-neutral_pitch_;
        if(boost_neutral_required_ && !arms_.active()) {
            const bool neutral=forward<.174533f;
            neutral_time_=neutral&&neutral_candidate_?neutral_time_+dt:0;neutral_candidate_=neutral;
            if(neutral_time_>=.12f)boost_neutral_required_=false;
        }
        if(arms_.active() || boost_neutral_required_ || compressed || crouch_ || jump_recovering_ || recovery_>0) {boost_=false;boost_candidate_=false;boost_time_=0;}
        else if(!boost_) {
            const bool tilted=forward>=.314159f;
            boost_time_=tilted&&boost_candidate_?boost_time_+dt:0;
            boost_candidate_=tilted;
            if(boost_time_>=.2f)boost_=true;
        } else if(forward<.174533f){boost_=false;boost_candidate_=false;boost_time_=0;}
        const auto frontal=[](const auto& left,const auto& right,float threshold) {
            const float x=left[0]-right[0],z=left[2]-right[2],span=std::hypot(x,z);
            return std::isfinite(span) && span>.12f && span<.8f && std::abs(x)/span>=threshold;
        };
        // A throw may finish in a forward/front-facing pose. Require a return
        // to riding stance before interpreting that lingering pose as braking.
        const bool valid_axes=frontal(p.left_shoulder,p.right_shoulder,0) && frontal(p.left_hip,p.right_hip,0);
        if(brake_side_required_ && !arms_.active()) {
            const bool riding=valid_axes && !frontal(p.left_shoulder,p.right_shoulder,.766044f) &&
                               !frontal(p.left_hip,p.right_hip,.766044f);
            side_time_=riding&&side_candidate_?side_time_+dt:0;side_candidate_=riding;
            if(side_time_>=.12f)brake_side_required_=false;
        }
        const float threshold=brake_ready_?.766044f:.866025f; // enter 30 deg, release 40 deg
        const bool front=frontal(p.left_shoulder,p.right_shoulder,threshold) &&
                         frontal(p.left_hip,p.right_hip,threshold) && !jump_protected() && !arms_.active() && !brake_side_required_;
        if(!front){brake_ready_=front_candidate_=false;front_time_=0;}
        else {
            front_time_=front_candidate_?front_time_+dt:0;front_candidate_=true;
            if(front_time_>=.2f)brake_ready_=true;
        }
    }
    void reset() {*this=CameraRaceMotion{};}
    bool ready() const {return ready_;}
    float lean() const {return lean_;}
    bool crouch() const {return crouch_;}
    bool jump() const {return jump_;}
    // Legacy name: ordinary forward board acceleration, not Air-consuming Kick Dash.
    bool boost() const {return boost_;}
    bool jump_protected() const {return crouch_candidate_||crouch_||jump_||jump_recovering_||recovery_>0;}
    bool brake_ready() const {return brake_ready_;}
    bool arm_action() const {return arms_.active();}
    bool kick_leg_action() const {return legs_.active();}
    bool overthrow() const {return overthrow_.fired();}
    void consume_overthrow(){overthrow_.reset();}
private:
    CameraArmActivity arms_;
    CameraLegActivity legs_;
    CameraOverthrow overthrow_;
    bool boost_neutral_required_=false,brake_side_required_=false,neutral_candidate_=false,side_candidate_=false;
    float neutral_time_=0,side_time_=0;
    bool jump_recovering_=false,rise_candidate_=false,brake_ready_=false,front_candidate_=false;
    float lowest_height_=0,previous_height_=0,rise_time_=0,front_time_=0;
    uint64_t sequence_=0;
    float elapsed_=0;
    bool ready_=false,crouch_=false,jump_=false,boost_=false,crouch_candidate_=false,boost_candidate_=false;
    float calibration_=0,anchor_roll_=0,anchor_pitch_=0,anchor_height_=0;
    float roll_sum_=0,pitch_sum_=0,height_sum_=0,neutral_roll_=0,neutral_pitch_=0,neutral_height_=0;
    float filtered_roll_=0,filtered_pitch_=0,filtered_height_=0,lean_=0;
    float crouch_time_=0,boost_time_=0,recovery_=0;
};
}
