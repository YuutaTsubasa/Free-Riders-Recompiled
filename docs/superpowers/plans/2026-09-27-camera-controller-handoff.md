# Camera/controller automatic handoff

**Goal:** Camera augments logical P1 without taking P1's configured controller away or freezing input on lost tracking. User selected automatic controller priority and return to camera after idle.

**Architecture:** A small clock-driven input selector chooses one complete P1 skeleton per sensor frame. Existing guest-based player routing writes that skeleton without changing identities. P2 always uses its configured controller index 1. Race overrides follow the published source; camera frames retain original gesture recognition. Debug renders the exact submitted P1 skeleton and names controller fallback/priority.

**Tech stack:** C++20, existing NUI hooks and Win32 debug view; portable tests and local Windows build.

## Rules

- Any mapped P1 button, trigger above 30, or stick beyond the existing 7849 dead zone takes priority immediately; keyboard mappings count too.
- Keep controller priority for 1500 ms after the last active sample, allowing release gestures and menu dwell to finish.
- A camera pose is usable only if its age is finite, nonnegative, and at most 500 ms. This tolerates brief missed detections but not a frozen stream. No pose means controller fallback.
- Source changes preserve logical-player binding, tracking ID, enrollment and P2 controls. Re-arm P1's simulated menu hand on camera-to-controller transition so intentional stick movement raises it again.
- Debug shows actual submitted joints, including simulated controller joints, and distinguishes source from camera tracking health.
- Preserve the existing shared race-preparation measurement skip: hybrid P1 still has a controller and P2 may be controller-only. This setup shortcut is separate from live camera body/gesture recognition.
- No new launcher source setting, no commits/PR/release in this local test iteration.

## Tasks

- [x] Add `CameraInputSelection` in `src/camera_input.h`; tests in `tests/camera_input_test.cpp` exercise startup, every input field, drift, hold timeout, brief loss, frozen feed and recovery. Run failing tests before implementing selector.
- [x] Integrate selection in `src/nui_hooks.cpp`: sample metadata even with Debug closed, always query P2 index 1, write selected P1 through routing, publish atomic camera-active state, re-arm only P1 on handoff. Read back submitted P1 joints for Debug. Add regression assertions for routing/fallback.
- [x] Add controller source status to `src/camera_debug.*`, window and tests.
- [x] Keep race body/gesture overrides only for pad-driven players; leave camera player on guest calculations. Retain P2 control during camera P1. Check preparation shortcut and special detector dispatch for source consistency.
- [x] Build targeted tests and diagnostic binary, run tests, review integration, stage verified executable in `out/camera-3d-play`, preserve diagnostic log. User validates START cursor and automatic handoff with their camera.

## Verification

13 targeted CTest targets passed, including 22 real race-player harness cases. Camera selector and routing regressions were observed failing before correction; race harness mutation rejected disabling camera ownership. Synthetic Win32 Debug probe passed rendering, source labels, close and modal shutdown. Camera-off Vulkan smoke reached the requested 180-present limit (expected exit 3). Both staged executable SHA-256 hashes match the local build. Real webcam START cursor and physical controller handoff still require user validation.
