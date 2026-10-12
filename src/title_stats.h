#pragma once
#include <cstdint>

namespace sfr {
// What the game window's title reports (menu_steady_hotkey.cpp), counted
// where it happens and taken every half second: how long the frames took,
// and how often a body arrived.

// A frame presented (guest_graphics_hooks.cpp): its time since the last one.
void note_frame_presented();
// A new Kinect skeleton frame the game read (nui_hooks.cpp).
void note_kinect_frame();
// A webcam picture the pose model read, and how long it took (camera_player.cpp).
void note_pose_estimate(double milliseconds);

// The game's own window (native_presentation.cpp), for its title: the
// window handle on Windows, null until it is made.
void set_game_window(void* window);
void* game_window();

struct TitleStats {
    double worst_frame_ms = 0;  // the longest frame since the last take
    uint32_t kinect_frames = 0;
    uint32_t pose_estimates = 0;
    double pose_ms = 0;          // their total time
};
// What was counted since the last take, and starts counting again.
TitleStats take_title_stats();
}
