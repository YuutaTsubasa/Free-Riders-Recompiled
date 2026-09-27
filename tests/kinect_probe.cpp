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
    std::string why;
    auto sensor = sfr::KinectSensor::open(&why);
    if (!sensor) {
        std::cout << "no sensor: " << why << '\n';
        return 1;
    }
    std::cout << "sensor open; stand in front of it\n";
    sfr::KinectFrame frame;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    uint64_t shown = 0;
    while (std::chrono::steady_clock::now() < end) {
        if (!sensor->next(frame)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (frame.number - shown < 15) continue;  // twice a second
        shown = frame.number;
        std::cout << "frame " << frame.number << " floor=" << frame.floor_plane[1] << ',' << frame.floor_plane[3]
                  << " bodies=" << frame.bodies.size();
        for (const auto& body : frame.bodies) {
            const auto& hand = body.joints[sfr::nui_joint::hand_right];
            std::cout << " [id " << body.tracking_id << " at " << body.position[0] << ',' << body.position[2]
                      << " right hand " << hand[0] << ',' << hand[1] << ',' << hand[2] << ']';
        }
        std::cout << '\n';
    }
    return 0;
}
