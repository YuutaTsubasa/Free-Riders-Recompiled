#include "kinect_stability.h"

namespace sfr {

KinectBodySteadying KinectBodySteadying::from_environment() {
    // The player's switch (menu_steadying) decides; it reads the environment.
    return KinectBodySteadying(true);
}

KinectBodySteadying::Body& KinectBodySteadying::body_for(uint32_t tracking_id) {
    for (Body& body : bodies_)
        if (body.tracking_id == tracking_id) return body;
    bodies_.push_back(Body{tracking_id, MenuHandSteadying(steady_menu_), 0});
    return bodies_.back();
}

void KinectBodySteadying::steady(std::vector<KinectBody>& bodies, bool in_menu) {
    ++frame_;
    if (!steady_menu_) return;
    for (KinectBody& body : bodies) {
        if (!body.tracking_id) continue;
        Body& memory = body_for(body.tracking_id);
        memory.last_seen = frame_;
        // A hand (or the shoulders it is measured from) the sensor did not
        // track is left as given and starts over: its position is stale or
        // zero, and steadying it would read it as a jump and rest there.
        // (Steadying moves a hand's wrist with it, so both are put back.)
        const uint32_t hands[2] = {nui_joint::hand_left, nui_joint::hand_right};
        const uint32_t wrists[2] = {nui_joint::wrist_left, nui_joint::wrist_right};
        const bool shoulders = body.joint_states[nui_joint::shoulder_center] != 0;
        const SkeletonJoints given = body.joints;
        memory.hands.steady(body.joints, in_menu);
        for (size_t side = 0; side < 2; ++side)
            if (!shoulders || body.joint_states[hands[side]] == 0) {
                body.joints[hands[side]] = given[hands[side]];
                body.joints[wrists[side]] = given[wrists[side]];
                memory.hands.forget_hand(side);
            }
    }
    // A body gone for a second is somebody else when that id comes back.
    std::erase_if(bodies_, [&](const Body& body) { return frame_ - body.last_seen > 30; });
}

}
