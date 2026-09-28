#pragma once
#include "kinect_sensor.h"

#include <memory>

namespace sfr {
// A window of what a real Kinect sees, for placing and tilting it before
// playing: the colour camera and the depth camera, each with the skeletons
// the sensor tracks drawn over it, and whether each body's feet are in view.
// It opens the sensor itself (the SDK allows one session a process) and
// shares it for the launcher's tilt buttons; closing the window, or
// destroying this, closes the sensor so the game can open it. Windows only:
// elsewhere open() returns null.
class KinectPreviewWindow {
public:
    static std::unique_ptr<KinectPreviewWindow> open(bool chinese);
    ~KinectPreviewWindow();
    bool closed() const;
    // The open sensor, or null while it is opening or when there is none.
    std::shared_ptr<KinectSensor> sensor() const;
    // Why no sensor opened (KinectSensor::open's reason, "no-runtime",
    // "no-sensor", ...), or empty while it opens or once it has.
    std::string failure() const;
    struct Impl;
private:
    explicit KinectPreviewWindow(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};

// Where a point of the sensor's skeleton space (metres) falls in one of its
// images: the depth camera's 320x240 exactly (the SDK's
// NuiTransformSkeletonToDepthImage), the colour camera's 640x480 roughly
// (its nominal focal length; the two cameras sit a few centimetres apart).
struct KinectImagePoint { float x = 0, y = 0; bool visible = false; };
KinectImagePoint kinect_depth_point(const std::array<float, 3>& p);
KinectImagePoint kinect_colour_point(const std::array<float, 3>& p);
}
