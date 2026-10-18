#include "touch_scroll.h"

#include <algorithm>
#include <cmath>

namespace sfr {

namespace {
// Coasting: the speed halves about every 0.17 s and ends below this.
constexpr float friction = 4.0f;
constexpr float min_speed = 30.0f;
// The release speed follows the last ~50 ms of the drag, so a finger that
// stopped before lifting does not fling the page.
constexpr float velocity_window = 0.05f;
}

TouchScroll::Step TouchScroll::update(bool down, float x, float y, float dt, bool may_drag) {
    Step step;
    if (dt < 0) dt = 0;
    switch (phase_) {
    case Phase::idle:
    case Phase::coasting:
        if (down) {
            // A press: also stops a coasting page, as a phone does.
            phase_ = Phase::pressed;
            origin_x_ = x;
            origin_y_ = y;
            last_y_ = y;
            velocity_ = 0;
        } else if (phase_ == Phase::coasting) {
            step.scroll = velocity_ * dt;
            velocity_ *= std::exp(-friction * dt);
            if (std::fabs(velocity_) < min_speed) stop();
        }
        break;
    case Phase::pressed: {
        if (!down) { phase_ = Phase::idle; break; }
        const float dx = x - origin_x_, dy = y - origin_y_;
        if (std::fabs(dx) >= threshold_ && std::fabs(dx) > std::fabs(dy)) {
            // Sideways first: a slider or a swipe of its own, not a scroll.
            phase_ = Phase::ignored;
        } else if (std::fabs(dy) >= threshold_ && may_drag) {
            phase_ = Phase::dragging;
            step.started = true;
            // From the threshold on, so the page does not jump.
            step.scroll = -(dy - (dy > 0 ? threshold_ : -threshold_));
            last_y_ = y;
        }
        break;
    }
    case Phase::ignored:
        if (!down) phase_ = Phase::idle;
        break;
    case Phase::dragging:
        if (!down) {
            phase_ = std::fabs(velocity_) >= min_speed ? Phase::coasting : Phase::idle;
            if (phase_ == Phase::idle) velocity_ = 0;
            break;
        }
        step.scroll = -(y - last_y_);
        last_y_ = y;
        if (dt > 0) {
            const float blend = std::min(1.0f, dt / velocity_window);
            velocity_ += (step.scroll / dt - velocity_) * blend;
        }
        break;
    }
    return step;
}

void TouchScroll::stop() {
    if (phase_ == Phase::coasting) phase_ = Phase::idle;
    velocity_ = 0;
}

}  // namespace sfr
