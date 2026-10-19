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

## Frame skipping instead of a variable race step (2026-10-15, 2026-10-16)

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
  real time. When it is more than a frame behind, the next frame runs the
  race without drawing it. At most four frames in a row go undrawn (the
  screen still shows at least 12 frames a second).
- A frame that is not drawn also leaves out most of the game's own drawing
  work (`src/race_task_hooks.cpp`, below), so it is far shorter than a drawn
  one and the race can catch up.
- When undrawn frames are no shorter than a frame (the game itself too slow,
  not its drawing), skipping cannot catch up: frames keep being drawn, tried
  again once a second. A race more than six frames behind (a load, a stall)
  starts the count again from now. In both cases the race slows down, as on
  the console, with its step unchanged.
- Menus always draw every frame.
- It is on everywhere by default (desktops too since 2026-10-16: a handheld
  PC or a slow GPU falls under 60 as a phone does); a device that keeps 60
  draws every frame. `SFR_RACE_FRAMESKIP=0` turns it off (the step then
  follows `SFR_REALTIME_RACE` as before). `SFR_RACE_FRAMESKIP=2` also skips
  when undrawn frames are a frame or longer, if they are under three quarters
  of a drawn one (Issues #176, #177; `SFR_TEST_RACE_SKIP_SLOW=1` is its old
  name). The launcher's Graphics > Advanced performance options > Frame
  skipping sets it: Auto (1), Always when behind (2), Off (0).
  `SFR_RACE_SKIP_DRAWING=0` keeps all of the game's drawing on undrawn
  frames (as 0.8.x).
- `game.log` has `RACE_FRAMESKIP mode=` (auto, always or off) once at the
  first frame, and `RACE_FRAMESKIP frames= skipped= speed=` every 600 race
  frames; `speed` is the race's frames against real time (1 is full speed).
- `SFR_TEST_RACE_DRAW_MS=<ms>` (testing) makes each drawn race frame take
  that much longer, as on a device whose GPU cannot keep 60.

### Logic and drawing in the game's frame

The title's task manager (`824A0458` → `824A00C8`) runs a frame as phases
over its task objects (about 300 in a race). Each task's methods 4 (handed to
the three job workers) and 8 are the race's logic and make no graphics calls;
its method 12, run in phases 4 and 8, is its drawing: about 13,600 graphics
calls and 10 of the 11 ms a race frame took on a PC. Skipping only the host's
drawing left all of that, so an undrawn frame still cost about 15 ms on an
AYN Thor and the race could not catch up.

The drawing cannot all be left out: some of what it writes, the logic reads
next frame. `SFR_TEST_DRAW_READS=1` (testing, Windows; races run about ten
times slower, `SFR_TEST_DRAW_FROM=<race frame>` starts it later) finds that:
after the drawing, every word it changed in the task objects and in the pages
the main thread wrote (heap and globals) is made unreadable until the next
frame's drawing, and each read is recorded with the reading instruction and
the word's owner (`DRAW_READS` lines; the instruction's offset names the
recompiled function through the link map). A word the logic writes first is
not counted. In a Free Race the readers were only the player racer's matrix
(`821A1920`), item-like objects' positions (`821A50xx`, `821A4F70`), the
drawing's own allocator and the guest's rendering thread. The other racers'
matrices are written again by the logic before it reads them.

So an undrawn frame leaves out the drawing of the 14 task kinds in
`undrawn_skips` (about 12 of 12.4 ms on a PC), and every other kind still
draws, those that are read among them, and any kind not seen in that race.

Measured on an AYN Thor, Free Race, `SFR_TEST_RACE_DRAW_MS` making drawn
frames slower:

| Drawn frames slowed by | Undrawn | Speed |
| --- | --- | --- |
| 0 (60 fps) | 5% | 1.00 |
| 12 ms, skipping the game's drawing | 57% | 1.00 |
| 12 ms, only the host's drawing (`SFR_RACE_SKIP_DRAWING=0`) | 54% | 0.75 |
| 30 ms, skipping the game's drawing | 76% | 0.99 |

An undrawn Thor frame now costs about 6-7 ms, most of it the race's logic
(2.1 ms in the job workers, 3.4 ms in method 8), which always runs.
