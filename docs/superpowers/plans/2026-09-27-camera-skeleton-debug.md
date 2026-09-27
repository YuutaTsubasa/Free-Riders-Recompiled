# Camera Skeleton Debug Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development. Keep the root dirty workspace untouched.

**Goal:** Optional Windows Camera Input debug window showing only the exact skeleton delivered to the guest, with no camera image.

**Architecture:** Capture/model processing stays unchanged. On the NUI frame submission path, publish a small latest-only snapshot of the 20 camera joints after writing them to guest memory. A separate Win32/GDI UI thread renders front and side views at at most 20 Hz. The game never waits for drawing, and closing the debug window does not close the game or stop camera tracking. Disabled mode creates no window or worker.

**Tech Stack:** C++20, Win32/GDI, existing launcher settings and CMake tests. Non-Windows builds retain a no-op window implementation and do not expose the Windows-only toggle.

## Confirmed scope
- User explicitly chose only the game-received skeleton, no camera preview.
- Launcher motion-camera settings gain persisted camera_debug=false by default and SFR_CAMERA_DEBUG=1 only when enabled with motion.
- Show 20 joints, colored left/right sides, front XY and side ZY projections in metres; no second inference or capture instance.
- Show waiting, live, held-last-pose and stale guest-update states. Do not imply synthetic webcam depth is measured depth.
- Use the CameraPlayer observation status to distinguish held game pose from a fresh detection. Publish only after write_joints succeeds.
- Initial supported UI platform Windows; local development build, no automatic PR, merge or release.

## Tasks
- [x] Settings/UI agent: launcher_settings.h/.cpp, launcher_main.cpp, launcher_settings_test.cpp and docs/camera-input.md. Test default, round-trip, motion-only environment gating. No CMake or runtime edits.
- [x] Parent: failing tests for snapshot status/projection/bone topology/finite data handling, then portable skeleton_debug model.
- [x] Parent: Windows window + non-Windows no-op implementation. Latest-snapshot mailbox, bounded redraw, independent close, safe shutdown.
- [x] Parent: camera status metadata and guest submission integration; build wiring and synthetic window probe.
- [x] Verify targeted tests, synthetic waiting/live/held rendering and close behavior; build launcher and native runtime with audited unchanged generated PPC objects.
- [x] Independent review; stage local camera test launcher and provide instructions. Real webcam tracing verification requires the user's camera/model setup.

## Verification
- Windows 7/7 targeted CTest checks passed.
- Synthetic Windows probe passed render snapshots, opt-in gating, independent close, immediate shutdown, and shutdown during modal move.
- Linux Clang 15 compiled and passed portable debug model/non-Windows stub and launcher settings tests.
- Release launcher and all native runtime sources built; 123 unchanged generated PPC objects reused (forced header differences are only unused function declarations).
- Independent code review found a modal-loop shutdown issue; fixed and reviewed again with no remaining significant findings.
- Manual real-camera test pending; local build in out/camera-debug-play at repository root.
- Staged native executable hash matches the final build; Vulkan startup reached 180 presents with camera disabled and stopped at the requested present limit.
