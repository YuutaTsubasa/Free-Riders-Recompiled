#pragma once
#include <chrono>
#include <cstdint>
#include <cstdlib>

namespace sfr {

// The motion trace (SFR_CAMERA_RACE_TRACE=1; the launcher's Motion trace
// switch): numeric lines in game.log about what a Kinect or a webcam player
// did and what the game made of it, never a camera image. Off by default.
//
//   KINECT_TRACE        a real Kinect's bodies, ten times a second
//   KINECT_COST         the time the Kinect's per-frame work took, every 300
//                       frames (about 5 s)
//   KINECT_MOTION       each crouch and jump the skeleton fallback saw
//                       (written without the trace too, at most 600 lines)
//   KINECT_GESTURE      the game's crouch/charge/jump/Side detectors' answer
//                       and the fallback's, when either changes
//   CAMERA_RACE_MOTION  the fallback's lean, crouch and jump, twice a second
//   CAMERA_RACE_ACTOR   the gesture flags the rider received and its state
//   CAMERA_GESTURE, CAMERA_RACE_JOINTS  the same for a webcam player
// Read when asked; a caller keeps the answer (a static) for the run.
inline bool motion_trace_enabled() {
    const char* value = std::getenv("SFR_CAMERA_RACE_TRACE");
    return value && *value == '1';
}

// At most per_second lines a second of one kind, so a detector whose answer
// flips every frame cannot fill the log. The lines held back are counted and
// reported on the next line that is written (dropped()).
class TraceRate {
public:
    using Clock = std::chrono::steady_clock;
    explicit TraceRate(uint32_t per_second) : per_second_(per_second) {}
    bool allow(Clock::time_point now) {
        if (window_ == Clock::time_point{} || now - window_ >= std::chrono::seconds(1)) {
            window_ = now;
            written_ = 0;
        }
        if (written_ >= per_second_) {
            ++held_back_;
            return false;
        }
        ++written_;
        dropped_ = held_back_;
        held_back_ = 0;
        return true;
    }
    bool allow() { return allow(Clock::now()); }
    // Lines held back since the previous one written, for the line allow()
    // just let through.
    uint64_t dropped() const { return dropped_; }
private:
    uint32_t per_second_;
    Clock::time_point window_{};
    uint32_t written_ = 0;
    uint64_t held_back_ = 0, dropped_ = 0;
};

// How long a piece of per-frame work took, for KINECT_COST: the calls since
// the last report, their mean and the longest, in microseconds.
class TraceCost {
public:
    void add(std::chrono::steady_clock::duration took) {
        const double us = std::chrono::duration<double, std::micro>(took).count();
        ++calls_;
        total_us_ += us;
        if (us > max_us_) max_us_ = us;
    }
    uint32_t calls() const { return calls_; }
    double mean_us() const { return calls_ ? total_us_ / calls_ : 0.0; }
    double max_us() const { return max_us_; }
    void clear() { *this = TraceCost{}; }
private:
    uint32_t calls_ = 0;
    double total_us_ = 0, max_us_ = 0;
};

}
