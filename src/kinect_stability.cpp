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
        memory.hands.steady(body.joints, in_menu);
    }
    // A body gone for a second is somebody else when that id comes back.
    std::erase_if(bodies_, [&](const Body& body) { return frame_ - body.last_seen > 30; });
}

}
