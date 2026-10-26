#pragma once
#include <cstdint>
#include <deque>
#include <optional>

namespace sfr {
// The timing of the achievement notification (Issue #288), as the console's
// system notification showed it: it rises into place while fading in, stays
// about four seconds, then fades out; several in a row follow each other.
struct ToastTiming {
    static constexpr double rise = 0.35, hold = 4.3, fade = 0.4, gap = 0.3;
    static constexpr double total = rise + hold + fade;
};
struct ToastFrame {
    uint16_t id = 0;
    float opacity = 0;  // 0..1
    float lift = 0;     // 0 in place, 1 a whole rise below it
};
class ToastQueue {
public:
    // Ids already queued or showing are not queued again.
    void push(uint16_t id);
    // What shows at `now` (seconds on any steady clock), starting the next
    // one once the previous has gone.
    std::optional<ToastFrame> at(double now);
    bool empty() const { return queue_.empty(); }
    // The one showing (or next), if any.
    std::optional<uint16_t> front() const;
private:
    std::deque<uint16_t> queue_;
    std::optional<double> started_;    // when the front one started
    std::optional<double> next_from_;  // the earliest the next may start
};
}
