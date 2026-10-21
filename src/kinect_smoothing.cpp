#include "kinect_smoothing.h"

#include <unordered_set>

namespace sfr {

void KinectJointSmoother::apply(KinectFrame& frame) {
    std::unordered_set<uint32_t> seen;
    for (auto& body : frame.bodies) {
        seen.insert(body.tracking_id);
        nui_smooth_skeleton(bodies_[body.tracking_id], body.joints, body.joint_states, parameters_);
    }
    for (auto it = bodies_.begin(); it != bodies_.end();)
        it = seen.count(it->first) ? std::next(it) : bodies_.erase(it);
}

}
