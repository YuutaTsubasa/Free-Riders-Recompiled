#pragma once
#include "kinect_record_raw.h"
#include "kinect_v2_depth.h"

#include <string>
#include <vector>

namespace sfr {

// Making the game's depth again from a raw Kinect v2 recording
// (kinect_record_raw.h; sfr_kinect_depth_resynth, docs/kinect-sensor.md),
// through the same view state (KinectDepthViewer) and the same synthesis
// (kinect_v2_synthesize_depth) as the live sensor: frame by frame, the body
// frame it carries goes to body_frame when it is a new one, then the depth
// frame steps the view and is made. Points come from the recording's rays
// for frames the live path had the sensor's mapper for, from the typical
// lens (as live) for the others.
//
// None of this is Windows', so it is tested everywhere (kinect_v2_depth_test).

// A raw frame's bodies and floor as the view keeps a body frame
// (KinectDepthViewer::bodies), and the other way for the recorder.
KinectFrame kinect_raw_body_frame(const KinectRawFrame& frame);
void kinect_raw_set_bodies(const KinectFrame& bodies, KinectRawFrame& frame);

// The fit view's stretch a raw recording's lens gives (the sensor's focal
// length when the header has it, the typical v2's otherwise).
float kinect_raw_fit_scale(const KinectRawHeader& header);

// Whether a header's rays look like camera space (+x to the image's right,
// +y up): the middle of the left column points left, of the top row up.
// flip_x / flip_y say which axis points the other way; false when the rays
// are unusable (zero or not finite).
bool kinect_raw_rays_orientation(const KinectRawHeader& header, bool& flip_x, bool& flip_y);

class KinectV2Resynth {
public:
    // settings.fit_scale 0 takes kinect_raw_fit_scale(header).
    KinectV2Resynth(const KinectDepthSettings& settings, const KinectRawHeader& header);
    KinectDepthStats make(const KinectRawFrame& frame, KinectImage& out);
    const KinectDepthViewer& viewer() const { return viewer_; }
    // A note about the rays (flipped, unusable), empty when all is well.
    const std::string& warning() const { return warning_; }
private:
    KinectDepthViewer viewer_;
    std::vector<float> rays_, points_;
    bool rays_usable_ = false;
    int64_t last_body_time_ = 0;
    std::string warning_;
};

}
