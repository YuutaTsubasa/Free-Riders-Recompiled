#include "window_resize.h"

#include <iostream>
#include <stdexcept>

// The render size following a resized window (Issue #365, window_resize.h).
namespace {
namespace wr = sfr::window_resize;
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void render_size_setting() {
    require(!wr::parse_render_size(nullptr) && !wr::parse_render_size(""), "no SFR_RENDER_SIZE: the size stays");
    require(wr::parse_render_size("0") == 0u, "0 is Original");
    require(wr::parse_render_size("100") == 100u && wr::parse_render_size("300") == 300u, "a percent of the window");
    for (const char* text : {"-1", "1.5", "100%", " 100", "abc", "801", "99999999999"})
        require(!wr::parse_render_size(text), "anything else leaves the size alone");
}

void percents() {
    // The launcher's own: Original is 100 whatever the window.
    require(wr::percent_for(1080, 0, 1280, 720) == 100 && wr::percent_for(2160, 0, 1280, 720) == 100,
            "Original keeps 720 lines");
    // 1.0x draws the window's lines: 720 -> 100, 1080 -> 150, 768 -> 107, 1440 -> 200.
    require(wr::percent_for(720, 100, 1280, 720) == 100, "1.0x at 720 lines");
    require(wr::percent_for(1080, 100, 1280, 720) == 150, "1.0x at 1080 lines");
    require(wr::percent_for(768, 100, 1280, 720) == 107, "1.0x at 768 lines");
    require(wr::percent_for(1080, 150, 1280, 720) == 225, "1.5x at 1080 lines");
    // Within 25..800, then lowered so that no side passes 8192.
    require(wr::percent_for(100, 25, 1280, 720) == 25, "never below 25");
    require(wr::percent_for(4320, 300, 1280, 720) == 640, "8192 / 1280 lowers 800 to 640");
    require(wr::percent_for(1080, 100, 2520, 720) == 150, "a wide frame at 1080 lines");
    require(sfr::render_scale::percent_of_output(1080, 100) == 150 && sfr::render_scale::percent_of_output(1080, 0) == 100,
            "the launcher's formula");
}

void settling() {
    const auto t0 = wr::Settle::clock::time_point{} + 1h;
    wr::Settle settle(1280, 720);
    require(!settle.update(1280, 720, false, t0), "the size it starts with needs nothing");
    // A drag: a new size each frame, nothing applied while it changes.
    require(!settle.update(1300, 730, false, t0 + 16ms), "a new size waits");
    require(!settle.update(1400, 760, false, t0 + 32ms), "and waits again when it changes");
    require(!settle.update(1400, 760, false, t0 + 200ms), "less than 200 ms since it last changed");
    const auto applied = settle.update(1400, 760, false, t0 + 232ms);
    require(applied && applied->first == 1400 && applied->second == 760, "settled for 200 ms: applied");
    require(!settle.update(1400, 760, false, t0 + 300ms), "applied once");
    // While Windows says the edge is held, a size that has not changed for a
    // while still waits; it is applied once the drag ends.
    require(!settle.update(1920, 1080, true, t0 + 1s), "dragging");
    require(!settle.update(1920, 1080, true, t0 + 2s), "still dragging, however long");
    const auto released = settle.update(1920, 1080, false, t0 + 2s + 16ms);
    require(released && released->second == 1080, "the drag ended: applied at once");
    // Minimized: no size, nothing changes, and back at the same size nothing is made.
    require(!settle.update(0, 0, false, t0 + 3s), "minimized");
    require(!settle.update(0, 1080, false, t0 + 4s), "a size of 0 is never applied");
    require(!settle.update(1920, 1080, false, t0 + 5s), "restored as it was: nothing to make");
    // There and back within the settle time: nothing.
    require(!settle.update(2560, 1080, false, t0 + 6s), "away");
    require(!settle.update(1920, 1080, false, t0 + 6s + 50ms), "and back");
    require(!settle.update(1920, 1080, false, t0 + 7s), "the applied size again: nothing");
    require(settle.applied() == std::pair<uint32_t, uint32_t>(1920, 1080), "what was applied");
}

void scripts() {
    using Action = wr::ScriptStep::Action;
    const auto steps = wr::parse_script("300:1920x1080,600:min,650:restore,900:full,1200:window,1500:1024x768");
    require(steps.size() == 6, "six steps");
    require(steps[0].present == 300 && steps[0].action == Action::size && steps[0].width == 1920 && steps[0].height == 1080,
            "a size");
    require(steps[1].action == Action::minimize && steps[2].action == Action::restore, "minimize and restore");
    require(steps[3].action == Action::fullscreen && steps[4].action == Action::windowed, "full screen and back");
    require(steps[5].present == 1500 && steps[5].width == 1024 && steps[5].height == 768, "the last size");
    const auto odd = wr::parse_script("10:1280x720,20:what,5:min,x:min,30:1280x,40:100x100,,50");
    require(odd.size() == 1 && odd[0].present == 10, "only the step that makes sense, in order");
    require(wr::parse_script("").empty(), "no script");
}
}

int main() {
    try {
        render_size_setting();
        percents();
        settling();
        scripts();
    } catch (const std::exception& error) {
        std::cerr << "window_resize_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "window_resize_test: ok\n";
    return 0;
}
