# Android performance in v0.4.4

The Android 10+ build has been tested on AYN Thor (Android 13, Adreno 740).
A complete race, steering, crouching, kick dash, and pause/resume were
verified. The user also confirmed that racing feels playable. Intro movies
remain slower than expected. Other tracks, modes and devices may behave
differently; this is still a preview.

## What changed

- Android worker threads keep their inherited CPU affinity, allowing the
  scheduler to select available fast cores. The previous fixed placement
  could leave a busy worker on a little core.
- Guest memory accesses within a committed partial page use an exact-bounds
  fast path. Thread-local state, register helpers, function lookup and ARM
  index conversion also do less repeated work.
- The known race clock uses elapsed host time for simulation and animation
  on Android, instead of assuming each rendered frame takes 1/60 second.
  This changes the game's time step, not just the displayed timer. Long
  stalls are bounded to a 250 ms step; perfect synchronization through a
  multi-second stall is not promised. Windows/Linux retain their prior
  timing behavior.
- Vulkan shaders represent GPU addresses with pairs of 32-bit words, so
  devices without `shaderInt64` can compile them. The address layout and
  D3D12 shader bytecode are preserved. Shader pack ABI is now **10**; keep
  the executable, shader pack and pipeline manifests from the same release.
- An unused Xbox Live presence update no longer reads a race object while
  it is being constructed. Offline gameplay and player profiles remain
  enabled.

## Measurements and limits

With normal audio, 720p rendering and the usual 60 FPS cap, the tested
Android 10+ build rendered about **23–24 FPS**. During one continuous race
segment, **64.85 game seconds elapsed in 64.79 real seconds**, measured
against monotonic present timestamps. A separate normal-build run measured
77.32 game seconds in 77.31 real seconds across two segments excluding a
pause; that measurement uses screenshot file timestamps and is less precise.
The character progressed through the course and completed the event.

These are device-specific observations, not a matched-scene performance
claim against Xenia or a guarantee for every race. Recording screenshots
and CPU profiles can affect performance. The shipped APK has no validation
layer, shell profiling permission, autoplay or diagnostic overrides.

Prefer the **android10-arm64** APK on Android 10 or later. The Android 9
compatibility APK uses emulated TLS and has not been tested on an Android 9
device. Updating over the existing installation preserves app data; do not
uninstall just to upgrade.

## Frame skipping instead of a variable race step (2026-10-15)

Until v0.8.3 an Android race stepped by the time that passed
(`SFR_REALTIME_RACE`), so a phone under 60 fps kept the race's speed. The
race's collisions were made for the console's step of a sixtieth of a second,
and some of its physics scale an impulse by the step: with longer steps a
fast racer could leave the course through the side of a ramp (Issue #123; on
desktops the same thing happened on Rocky Ridge until v0.5.3 made the step
fixed again).

Now a race keeps the fixed step on Android too, and keeps its speed by not
drawing some frames instead (`RaceFrameSkip` in `src/race_frame_clock.h`):

- At the end of each race frame the game compares the frames it has run with
  real time. When it is more than half a frame behind, the next frame runs
  the race without drawing it (no draws, resolves or present: the same frames
  `SFR_RENDER_EVERY` skips). At most two frames in a row go undrawn.
- A race more than six frames behind (a load, a stall, or a phone too slow to
  run the race itself at 60) starts the count again from now: the race then
  slows down, as on the console, rather than leaving the screen frozen.
- Menus always draw every frame.
- `SFR_RACE_FRAMESKIP=0` turns it off (the step then follows
  `SFR_REALTIME_RACE` as before); `=1` turns it on on a desktop. Desktops
  keep it off by default: the launcher holds them at 60 fps.
- `game.log` has `RACE_FRAMESKIP frames= skipped= speed=` every 600 race
  frames; `speed` is the race's frames against real time (1 is full speed).
- `SFR_TEST_RACE_DRAW_MS=<ms>` (testing) makes each drawn race frame take
  that much longer, as on a device whose GPU cannot keep 60.

On a desktop with `SFR_TEST_RACE_DRAW_MS=12` (drawn frames about 25 ms), the
race kept `speed=0.99` with 58% of its frames undrawn, and the in-game timer
read the same at the same frame as a race drawing every frame. With nothing
slowed down, the skips were at the start of the race (its first seconds load
and prepare shaders) and none after.

Skipping saves what drawing costs: the host's draw recording and the GPU's
work. The game's own frame (its logic and the draw list it builds) still runs
every frame, so a phone that is slow there rather than in the GPU gains less.
