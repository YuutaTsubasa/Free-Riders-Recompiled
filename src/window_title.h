#pragma once

namespace sfr {
// The game window's title says how it runs, every half second: the frame
// rate and frame times, the graphics, and how often a Kinect's skeletons or
// the webcam's poses arrive (title_stats.h). SFR_TITLE_STATS=0 (the
// launcher's Performance switch) leaves it alone. Called from the input
// update; nothing where there is no window.
void poll_window_title();
}
