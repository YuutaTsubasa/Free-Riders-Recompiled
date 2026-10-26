#include "achievement_toast.h"
#include <algorithm>

namespace sfr {
void ToastQueue::push(uint16_t id) {
    if (std::find(queue_.begin(), queue_.end(), id) == queue_.end()) queue_.push_back(id);
}

std::optional<uint16_t> ToastQueue::front() const {
    if (queue_.empty()) return std::nullopt;
    return queue_.front();
}

std::optional<ToastFrame> ToastQueue::at(double now) {
    while (!queue_.empty()) {
        if (!started_) {
            if (next_from_ && now < *next_from_) return std::nullopt;
            started_ = now;
        }
        const double t = now - *started_;
        if (t >= ToastTiming::total) {
            queue_.pop_front();
            next_from_ = *started_ + ToastTiming::total + ToastTiming::gap;
            started_.reset();
            continue;
        }
        ToastFrame frame;
        frame.id = queue_.front();
        if (t < ToastTiming::rise) {
            const double k = t / ToastTiming::rise;
            const double eased = 1.0 - (1.0 - k) * (1.0 - k) * (1.0 - k);  // ease out
            frame.opacity = float(k);
            frame.lift = float(1.0 - eased);
        } else if (t < ToastTiming::rise + ToastTiming::hold) {
            frame.opacity = 1.0f;
        } else {
            frame.opacity = float(1.0 - (t - ToastTiming::rise - ToastTiming::hold) / ToastTiming::fade);
        }
        frame.opacity = std::clamp(frame.opacity, 0.0f, 1.0f);
        return frame;
    }
    return std::nullopt;
}
}
