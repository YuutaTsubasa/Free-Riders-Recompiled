# Android Intro decoder performance (2026-10-01)

## Symptom and evidence

On the connected AYN Thor (Android 13, Adreno 740), v0.4.4 racing was
accepted as playable, but Intro video could still fall behind its music.
The owned `titleMovie_j.wmv` is 1280x720 WMV3 at 30000/1001 fps; its container
duration is 72.414 seconds. No game assets are included here.

A private observational build retained the existing blocking
`RenderNextFrame` compatibility policy and ran every original call once.
It added the frame timestamp at player +288 to the existing `MOVIE_TRACE`
windows, and measured decoder-ready waits (`828266C8`) separately from guest
sleeps (`824D0148`). The guest decoder copies each frame's timestamp into
+288 in `82827930`; observed timestamps advance in milliseconds, including
4004, 10010, and 72072. Success return counts alone are not decoded-frame
or audio-sync evidence.

The first baseline spent about 37.4 seconds in decoder-ready waits over a
74.7-second movie, but only 49 ms in the instrumented sleep helper. A
30-second `simpleperf` CPU-clock capture attributed 63% of process CPU time
to the video decoder thread: 27.7 CPU seconds, mostly on the prime core.
Partial vector reads accounted for 13.3% of that thread's sampled leaf time.
This identifies CPU decode throughput, rather than extra frame pacing or
GPU pipeline compilation, as a concrete bottleneck in this run.

## Change

Only AArch64 vector-memory helpers change:

- Partial vector loads use a NEON table lookup to reverse and zero-fill
  lanes when the entire aligned block is validated as ordinary committed
  memory. The vector read is explicitly volatile. Short mappings, special
  words, and excluded providers retain the original exact-range fallback;
  an empty right load still does not access memory.
- Partial vector stores validate their exact range once with `fast_write`,
  then retain the original volatile byte writes and order. Live reservations,
  pending I/O, special words, dirty-page tracking, and write epochs keep
  their existing handling. Other architectures keep the previous path,
  including Windows x64 write-combined completion fences.

No video frames are skipped, no playback or game clock is changed, and the
diagnostic hooks and profiling permissions are absent from the normal APK.

## Device measurements

The same full Intro ran to `XMV_ENDOFFILE` in every capture, with 2170
RenderNextFrame calls and 2169 successful returns. All used the same shader
pack and settings. Captures are local under `out/android-intro-20261001`.

| Capture | CPU sampling | First call to EOF | Maximum sampled video timestamp lag |
| --- | --- | ---: | ---: |
| v0.4.4 baseline (`pts`) | 30 seconds | 74.661 s | 4.332 s |
| NEON loads only (`neon`) | 30 seconds | 72.573 s | 2.692 s |
| NEON loads + checked-once stores (`stores`) | Off | 72.550 s | 0.724 s |
| v0.4.4 baseline repeated afterward (`baseline-repeat`) | Off | 76.856 s | 5.891 s |

Lag here is elapsed monotonic time since the first render call minus the
observed video timestamp. It includes startup latency and is measured at
the trace's periodic windows, not continuously. It is **not** a measurement
of audio-device presentation time or a proof of sample-accurate A/V sync.
Thermals and scheduling were not fixed, so these are single-device evidence,
not a general speedup percentage. The repeated unprofiled baseline also
falls behind, ruling out CPU sampling as the sole cause of the symptom.

## Validation

- Android ARM64 vector-memory executable passes on the connected device.
- Windows vector-memory, guest-memory, guest-entry-state and race-frame-clock
  CTests pass.
- Tests cover all lane offsets, zero fill, merged unaligned loads, address
  edges, short logical mappings, selected/excluded special words, reservation
  rejection, pending I/O, and dirty-page/epoch notifications.
- Independent review caught and resolved volatile-load semantics, Windows
  completion-fence preservation, and contamination of a write-watch test
  by an earlier rejected write.

- The normal APK completed a one-lap Speed Showdown and reached its result
  screen. At presents 12000 and 12400 the HUD read 1:07.97 and 1:24.47;
  Android screenshot file timestamps differ by 16.484 seconds. That is
  16.50 game seconds over 16.484 wall seconds (about 24.3 rendered fps in
  this interval). File-write timestamps are less precise than monotonic
  presentation telemetry, but show no return to frame-count-based slow motion.
  The scripted late inputs occurred after the finish, so this run does not
  independently revalidate every race gesture or pause/resume interaction.
- Packaged assets and dependent native libraries match v0.4.4 byte-for-byte;
  only `libmain.so` differs. The installed normal APK hash was checked against
  the tested file, and `debug.env` was removed afterward.

The existing limitations remain: this does not establish performance on
every Android device, nor does it remove all possible Intro lag.
