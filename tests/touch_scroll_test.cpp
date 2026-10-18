#include "touch_scroll.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }
constexpr float frame = 1.0f / 60.0f;
using sfr::TouchScroll;
}

int main() {
    try {
        {
            // A tap: down, a little wobble, up. Never a drag, never a scroll.
            TouchScroll touch(12);
            require(!touch.update(true, 100, 100, frame, true).started, "a press is not a drag");
            require(touch.phase() == TouchScroll::Phase::pressed, "a press is pressed");
            const auto wobble = touch.update(true, 104, 108, frame, true);
            require(!wobble.started && wobble.scroll == 0, "moving less than the threshold is still a tap");
            touch.update(false, 104, 108, frame, true);
            require(touch.phase() == TouchScroll::Phase::idle, "lifting ends the tap");
            require(touch.update(false, 104, 108, frame, true).scroll == 0, "a tap does not coast");
        }
        {
            // A drag up: the page scrolls down by the distance past the threshold, then 1:1.
            TouchScroll touch(12);
            touch.update(true, 100, 300, frame, true);
            const auto start = touch.update(true, 100, 280, frame, true);
            require(start.started, "past the threshold the press becomes a drag");
            require(near(start.scroll, 8), "the first scroll counts from the threshold, without a jump");
            const auto more = touch.update(true, 100, 250, frame, true);
            require(!more.started && near(more.scroll, 30), "then the page follows the finger");
            const auto back = touch.update(true, 100, 260, frame, true);
            require(near(back.scroll, -10), "and back the other way");
        }
        {
            // A drag down scrolls up.
            TouchScroll touch(12);
            touch.update(true, 50, 100, frame, true);
            const auto start = touch.update(true, 50, 130, frame, true);
            require(start.started && near(start.scroll, -18), "dragging down scrolls up");
        }
        {
            // Sideways first (a slider): never a scroll, even if it goes up later.
            TouchScroll touch(12);
            touch.update(true, 100, 100, frame, true);
            touch.update(true, 130, 105, frame, true);
            require(touch.phase() == TouchScroll::Phase::ignored, "a sideways move is left alone");
            const auto later = touch.update(true, 130, 20, frame, true);
            require(!later.started && later.scroll == 0, "it stays left alone until the finger lifts");
            touch.update(false, 130, 20, frame, true);
            require(touch.phase() == TouchScroll::Phase::idle, "lifting ends it");
        }
        {
            // A held slider or scrollbar: no drag while it is not allowed.
            TouchScroll touch(12);
            touch.update(true, 100, 100, frame, true);
            const auto held = touch.update(true, 100, 40, frame, false);
            require(!held.started && held.scroll == 0, "no drag while the press belongs to a control");
            require(touch.phase() == TouchScroll::Phase::pressed, "it waits");
        }
        {
            // A fling coasts on after release, slows down and stops.
            TouchScroll touch(12);
            float y = 600;
            touch.update(true, 100, y, frame, true);
            for (int i = 0; i < 10; ++i) touch.update(true, 100, y -= 20, frame, true);
            require(touch.velocity() > 600, "a fast drag has speed");
            touch.update(false, 100, y, frame, true);
            require(touch.phase() == TouchScroll::Phase::coasting, "a fling coasts");
            const auto coast = touch.update(false, 100, y, frame, true);
            require(coast.scroll > 5, "the page keeps moving after the finger lifts");
            float total = coast.scroll, previous = coast.scroll;
            int frames = 0;
            while (touch.phase() == TouchScroll::Phase::coasting && frames < 600) {
                const auto step = touch.update(false, 100, y, frame, true);
                require(step.scroll <= previous + 0.001f, "coasting slows down");
                previous = step.scroll;
                total += step.scroll;
                ++frames;
            }
            require(touch.phase() == TouchScroll::Phase::idle, "coasting ends");
            require(frames < 180, "within a few seconds");
            require(total > 50 && total < 1000, "a fling carries a sensible distance");
        }
        {
            // A drag that stopped before lifting does not fling.
            TouchScroll touch(12);
            touch.update(true, 100, 600, frame, true);
            touch.update(true, 100, 500, frame, true);
            for (int i = 0; i < 20; ++i) touch.update(true, 100, 500, frame, true);
            touch.update(false, 100, 500, frame, true);
            require(touch.phase() == TouchScroll::Phase::idle, "a still finger lifts without a fling");
        }
        {
            // Touching a coasting page stops it, and stop() ends a coast at the page's end.
            TouchScroll touch(12);
            float y = 600;
            touch.update(true, 100, y, frame, true);
            for (int i = 0; i < 10; ++i) touch.update(true, 100, y -= 20, frame, true);
            touch.update(false, 100, y, frame, true);
            require(touch.phase() == TouchScroll::Phase::coasting, "coasting");
            const auto caught = touch.update(true, 100, y, frame, true);
            require(caught.scroll == 0 && touch.phase() == TouchScroll::Phase::pressed, "a press stops the coast");
            touch.update(true, 100, y -= 40, frame, true);
            for (int i = 0; i < 5; ++i) touch.update(true, 100, y -= 20, frame, true);
            touch.update(false, 100, y, frame, true);
            require(touch.phase() == TouchScroll::Phase::coasting, "coasting again");
            touch.stop();
            require(touch.phase() == TouchScroll::Phase::idle && touch.velocity() == 0, "stop ends the coast");
            require(touch.update(false, 100, y, frame, true).scroll == 0, "and nothing moves after");
        }
        std::cout << "touch scroll tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "touch scroll test failed: " << error.what() << "\n";
        return 1;
    }
}
