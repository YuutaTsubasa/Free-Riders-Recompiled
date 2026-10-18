// Drag-to-scroll for a finger, as on a phone list (issue #186): a press that
// moves further than a small threshold, mostly up or down, becomes a drag
// that scrolls the page; a press that does not is left alone as a tap. After
// the finger lifts, the page coasts on and slows down.
// Plain state, no ImGui: the launcher feeds it the finger and applies the
// scroll it returns.
#pragma once

namespace sfr {

class TouchScroll {
public:
    enum class Phase { idle, pressed, ignored, dragging, coasting };

    struct Step {
        bool started = false;  // the press just became a drag: cancel the tap
        float scroll = 0;      // how far to scroll the page down this frame
    };

    // threshold: how far (in pixels) a finger moves before a press is a drag.
    explicit TouchScroll(float threshold = 12.0f) : threshold_(threshold) {}

    void set_threshold(float threshold) { threshold_ = threshold; }
    Phase phase() const { return phase_; }
    bool dragging() const { return phase_ == Phase::dragging; }
    float velocity() const { return velocity_; }

    // One frame. down: a finger is on the screen at (x, y); dt: seconds since
    // the last frame. may_drag: whether the press may become a drag now (a
    // slider or a scrollbar being held keeps the finger for itself).
    Step update(bool down, float x, float y, float dt, bool may_drag);

    // The page reached its end, or it is gone: no more coasting.
    void stop();

private:
    float threshold_;
    Phase phase_ = Phase::idle;
    float origin_x_ = 0, origin_y_ = 0;
    float last_y_ = 0;
    float velocity_ = 0;  // pixels per second, positive = scrolling down
};

}  // namespace sfr
