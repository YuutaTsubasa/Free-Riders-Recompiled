#pragma once
// The race's stereo test view and first-person camera (Issue #460, the first
// step toward a VR mode, without a headset): see race_stereo.cpp. The task
// manager's hook (race_task_hooks.cpp) asks begin() before a race frame's 3D
// drawing (phase key 4, round 0), draws that phase's tasks once for each of
// passes(), each after pass(i), and calls end() before the frame goes on.
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
// SFR_TEST_STEREO_SHOT (testing): whether this frame's 3D is also drawn once
// as the title draws it before the eyes, each written out before the
// full-frame passes (shot), to compare an eye with the same frame in mono.
bool shot_due();
void shot(const char* what);
}
