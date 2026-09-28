#include "kinect_preview.h"

namespace sfr {
// NuiImageCamera.h: NUI_CAMERA_DEPTH_NOMINAL_FOCAL_LENGTH_IN_PIXELS (at
// 320x240) and NUI_CAMERA_COLOR_NOMINAL_FOCAL_LENGTH_IN_PIXELS (at 640x480).
KinectImagePoint kinect_depth_point(const std::array<float, 3>& p) {
    if (!(p[2] > 0.1f)) return {};
    return {160.0f + p[0] * 285.63f / p[2], 120.0f - p[1] * 285.63f / p[2], true};
}
KinectImagePoint kinect_colour_point(const std::array<float, 3>& p) {
    if (!(p[2] > 0.1f)) return {};
    return {320.0f + p[0] * 531.15f / p[2], 240.0f - p[1] * 531.15f / p[2], true};
}
}
