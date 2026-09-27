#pragma once
#include "native_input.h"
#include <chrono>
#include <cmath>
#include <optional>

namespace sfr {
inline bool camera_controls_player(uint32_t player,bool camera_active) {
    return player==0 && camera_active;
}

class CameraInputSelection {
public:
    using Clock=std::chrono::steady_clock;
    bool update(const GamepadState& pad,double pose_age_ms,Clock::time_point now) {
        // Use the same stick dead zone as the emulated menu skeleton. The
        // mapped state also includes keyboard controls when configured.
        const auto moved=[](int16_t axis) {return std::abs(int(axis))>7849;};
        if(pad.buttons || pad.left_trigger>30 || pad.right_trigger>30 ||
           moved(pad.thumb_lx) || moved(pad.thumb_ly) || moved(pad.thumb_rx) || moved(pad.thumb_ry))
            last_controller_=now;
        const bool controller=last_controller_ && now-*last_controller_<std::chrono::milliseconds(1500);
        return !controller && std::isfinite(pose_age_ms) && pose_age_ms>=0 && pose_age_ms<=500;
    }
private:
    std::optional<Clock::time_point> last_controller_;
};
}
