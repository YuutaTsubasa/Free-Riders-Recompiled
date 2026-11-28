#pragma once
// The race's stereo test view and first-person camera (Issue #460, the first
// step toward a VR mode, without a headset): see race_stereo.cpp. The task
// manager's hook (race_task_hooks.cpp) asks begin() before a race frame's 3D
// drawing (phase key 4, round 0) and draws that phase's tasks once for each
// of passes(), each after pass(i). In a stereo frame the left eye's
// full-frame passes (key 8) follow its 3D at once, then keep_left_eye();
// the right eye's run where the title runs them. end() comes after key 8.
#include <cstdint>

struct PPCContext;

namespace sfr::race_stereo {
// SFR_TEST_STEREO=1 or SFR_TEST_FIRST_PERSON=1 (the launcher's Test options).
bool enabled();
// Whether this frame's 3D drawing is drawn by the passes below: a race frame
// with one view (not a two-player split) whose camera is the title's scene
// camera. False leaves it to the title (one pass, nothing changed).
bool begin(PPCContext& ctx, uint8_t* base);
int passes();
void pass(PPCContext& ctx, uint8_t* base, int index);
// Puts the camera, the view layout and the view's state back as they were.
void end(PPCContext& ctx, uint8_t* base);
// Whether this frame is drawn as two eyes (begin true, stereo on).
bool stereo_frame();
// After the left eye's full-frame passes: its frame is kept for the present.
void keep_left_eye();
// While the left eye's full-frame passes run: the UI timelines they update
// (824AE2A0, race_frame_clock_hooks.cpp) are evaluated without advancing,
// so the HUD's animations step once a frame, in the right eye's passes, as
// in a frame drawn once.
void hold_ui(bool on);
bool holding_ui();
// The eye being drawn in a stereo frame (0 left, 1 right; 0 otherwise).
uint32_t current_eye();
// The current eye's HUD shift in clip space (0 outside a stereo frame), for
// the race HUD's draws (guest_graphics_hooks.cpp).
float hud_shift();
}
