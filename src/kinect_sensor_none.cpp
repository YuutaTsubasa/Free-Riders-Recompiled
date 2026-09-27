#include "kinect_sensor.h"

namespace sfr {

bool KinectSensor::supported() { return false; }

std::unique_ptr<KinectSensor> KinectSensor::open(std::string* why) {
    if (why) *why = "unsupported-platform";
    return nullptr;
}

}
