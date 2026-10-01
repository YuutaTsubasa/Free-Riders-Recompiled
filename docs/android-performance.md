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
