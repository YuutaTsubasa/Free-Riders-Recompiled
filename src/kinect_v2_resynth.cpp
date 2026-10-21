#include "kinect_v2_resynth.h"
#include "kinect_v2_image.h"

#include <cmath>

namespace sfr {

KinectFrame kinect_raw_body_frame(const KinectRawFrame& frame) {
    KinectFrame out;
    out.floor_plane = frame.floor;
    // A v2's gravity is its floor's normal.
    out.gravity = {frame.floor[0], frame.floor[1], frame.floor[2]};
    for (const KinectRawBody& raw : frame.bodies) {
        if (!raw.tracked) continue;
        KinectBody& body = out.bodies.emplace_back();
        body.tracking_id = uint32_t(raw.tracking_id);
        body.sensor_index = raw.body_index;
        body.position = raw.spine_base;
        body.joint_states[0] = raw.spine_state;
    }
    return out;
}

void kinect_raw_set_bodies(const KinectFrame& bodies, KinectRawFrame& frame) {
    frame.floor = bodies.floor_plane;
    frame.bodies.clear();
    for (const KinectBody& body : bodies.bodies) {
        if (frame.bodies.size() >= kinect_raw_max_bodies) break;
        KinectRawBody& raw = frame.bodies.emplace_back();
        raw.tracking_id = body.tracking_id;
        raw.body_index = uint8_t(body.sensor_index);
        raw.tracked = 1;
        raw.spine_state = uint8_t(body.joint_states[0]);
        raw.spine_base = body.position;
    }
}

float kinect_raw_fit_scale(const KinectRawHeader& header) {
    const float focal = header.lens[1];
    return kinect_v2_fit_scale(focal > 100.0f && focal < 1000.0f ? focal : kinect_v2_typical_depth_focal);
}

bool kinect_raw_rays_orientation(const KinectRawHeader& header, bool& flip_x, bool& flip_y) {
    flip_x = flip_y = false;
    const size_t pixels = size_t(header.width) * header.height;
    if (header.rays.size() != 2 * pixels || !header.width || !header.height) return false;
    const size_t left = size_t(header.height / 2) * header.width, top = header.width / 2;
    const float x = header.rays[2 * left], y = header.rays[2 * top + 1];
    if (!std::isfinite(x) || !std::isfinite(y) || x == 0.0f || y == 0.0f) return false;
    flip_x = x > 0.0f;
    flip_y = y < 0.0f;
    return true;
}

KinectV2Resynth::KinectV2Resynth(const KinectDepthSettings& settings, const KinectRawHeader& header)
    : viewer_(settings), rays_(header.rays) {
    if (!(settings.fit_scale > 0)) viewer_.set_fit_scale(kinect_raw_fit_scale(header));
    bool flip_x = false, flip_y = false;
    rays_usable_ = kinect_raw_rays_orientation(header, flip_x, flip_y);
    if (!rays_usable_) {
        warning_ = "the recording's rays are unusable: every frame through a typical v2's lens";
    } else if (flip_x || flip_y) {
        // Camera space has +x to the image's right and +y up; a table the
        // other way round is turned so the points are the mapper's.
        warning_ = std::string("the recording's rays point ") + (flip_x ? "x" : "") + (flip_x && flip_y ? " and " : "") +
                   (flip_y ? "y" : "") + " the other way: turned to camera space";
        for (size_t i = 0; i < rays_.size(); i += 2) {
            if (flip_x) rays_[i] = -rays_[i];
            if (flip_y) rays_[i + 1] = -rays_[i + 1];
        }
    }
}

KinectDepthStats KinectV2Resynth::make(const KinectRawFrame& frame, KinectImage& out) {
    // A body frame the live path had not had before this image.
    if (frame.body_time != 0 && frame.body_time != last_body_time_) {
        viewer_.body_frame(kinect_raw_body_frame(frame), double(frame.body_time) * 1e-7);
        last_body_time_ = frame.body_time;
    }
    const KinectDepthView view = viewer_.depth_frame(double(frame.depth_time) * 1e-7);
    const size_t pixels = frame.depth.size();
    if ((frame.flags & kinect_raw_flag_mapped) && rays_usable_ && rays_.size() == 2 * pixels)
        kinect_v2_depth_points_from_rays(frame.depth.data(), rays_.data(), pixels, points_);
    else
        kinect_v2_depth_points_from_lens(frame.depth.data(), kinect_v2_depth_width, kinect_v2_depth_height, points_);
    const KinectDepthStats stats = kinect_v2_synthesize_depth(
        points_.data(), frame.body_index.size() == pixels ? frame.body_index.data() : nullptr, pixels, view, out);
    out.number = frame.number;
    out.steady_us = frame.steady_us;
    out.sdk_time = frame.depth_time;
    out.view_origin = view.origin;
    out.view_z_scale = view.z_scale;
    return stats;
}

}
