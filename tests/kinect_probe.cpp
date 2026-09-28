// Checks a real Kinect without starting the game: opens the first sensor
// through the Kinect for Windows runtime and prints what it tracks for ten
// seconds (sfr_kinect_probe [seconds]).
#include "kinect_sensor.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 10;
    // sfr_kinect_probe [seconds] [placement]: with a placement ("right",
    // "front-left", ...) the bodies are turned as the game turns them, and
    // which side of each the sensor is taken to see is printed.
    sfr::KinectPlacementTransform placement(sfr::kinect_placement_from(argc > 2 ? argv[2] : nullptr));
    std::string why;
    auto sensor = sfr::KinectSensor::open(&why);
    if (!sensor) {
        std::cout << "no sensor: " << why << '\n';
        return 1;
    }
    std::cout << sensor->model() << " open; stand in front of it" << std::endl;
    sfr::KinectFrame frame;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    uint64_t shown = 0;
    while (std::chrono::steady_clock::now() < end) {
        if (!sensor->next(frame)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        placement.apply(frame);
        if (frame.number - shown < 15) continue;  // twice a second
        shown = frame.number;
        std::cout << "frame " << frame.number << " floor=" << frame.floor_plane[1] << ',' << frame.floor_plane[3]
                  << " bodies=" << frame.bodies.size();
        for (const auto& body : frame.bodies) {
            const auto& hand = body.joints[sfr::nui_joint::hand_right];
            std::cout << " [id " << body.tracking_id << " at " << body.position[0] << ',' << body.position[2]
                      << " right hand " << hand[0] << ',' << hand[1] << ',' << hand[2]
                      << (placement.placement() == sfr::KinectPlacement::front ? ""
                          : placement.sees_back(body.tracking_id) ? " sees back" : " sees chest") << ']';
        }
        std::cout << std::endl;
    }
    return 0;
}
