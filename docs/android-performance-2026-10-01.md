# Pocket S2 Pro: Intro CPU investigation, 2026-10-01

The pipeline-preparation test APK still played both movies and races slowly on
the user's Pocket S2 Pro (Android 14, arm64, Adreno 750). Startup successfully
prepared all known pipelines. Slow race frames in the user's log had no new
pipeline compilation and substantial guest scheduling/blocked time. Pipeline
preparation therefore did not address the remaining sustained slowdown.

## Two isolated changes

1. The latest test package used API 28, which selected emulated TLS. The project
   already supports `SFR_ANDROID_API=29`, and earlier Android performance work
   used that option. Rebuild the current source for API 29 and package with
   `--min-sdk 29`. This private test requires Android 10+; the script's default
   API 28 and published compatibility policy are unchanged.
2. Portable events and semaphores previously notified a single global condition
   variable. Every event woke unrelated waiting threads. Keep the shared state
   mutex, but register each blocked wait with its object span and its own
   condition variable. Notify only waits containing the signaled object.
   Registration, notification and removal use the same mutex. Fast polls do not
   allocate/register. Wait-all consumption, deadlines and cancellation retain
   their existing semantics. Windows uses native synchronization objects and
   does not use this portable implementation in the game.

## Measurement

Same installed game data, settings, Vulkan, 720p/100%, guest workers=`cores`,
movies enabled, warmed pipeline cache, automatic launch, 1,600-present limit.
Movie throughput below sums `MOVIE_TRACE` windows with `total_calls` 31 through
601 (600 movie-update calls). The game clock/pacing policy is unchanged.

| Build | With CPU sampling | Without CPU sampling |
| --- | ---: | ---: |
| API 28, global event broadcast | 24.83 movie updates/s | 24.98 |
| API 29, global event broadcast | 27.65 | 27.64 |
| API 29, targeted event wakeups | 28.06 | 28.04 |

This is approximately 12% higher Intro movie throughput for the combined
candidate. Most of the measured gain comes from the native TLS build.
The small additional throughput difference from targeted wakeups is not strong
evidence of a general FPS gain: the separate present-frame window even varied
from 34.91 to 35.79 ms without sampling. Do not extrapolate these numbers to
3D races or promise full-speed playback. No repeated race input/replay was
available for this run, and CPU frequencies/temperature were not held constant.

Each profile used Android simpleperf `cpu-clock`, 400 Hz, call stacks, 30 s,
explicitly targeting the `com.freeriders.recompiled:game` PID. Targeting only
the package name instead captured the idle Launcher and was discarded.
Profiles contained 20,979 / 19,844 / 19,341 samples respectively, with zero lost
samples. Kernel symbols were restricted; userspace stacks still identified
their syscall callers. No device security settings were changed.

- API 28 TLS lookup (`__emutls_get_address` plus `pthread_getspecific`) consumed
  about 12.8% of sampled CPU. API 29 uses ELF TLS (verified PT_TLS); its dynamic
  TLS resolver still cost 5.8% in the isolated TLS run.
- The API 29 baseline spent 7.40% of sampled CPU in kernel work under event
  broadcast and 6.11% under portable wait-any's futex wait. With targeted
  wakeups, the event broadcast stack fell to about 1.00%.
- Total kernel CPU sample share fell from 34.67% to 21.57%. These are CPU sample
  shares, not frame wall time or directly additive speedups. The faster build
  progressed farther through the Intro during the fixed 30-second profile.
- Measured movie windows had zero pipeline-compilation and GPU-wait time.
  Guest CPU work and scheduling remain the main leads, rather than shader
  preparation or lowering render resolution for the movie slowdown.

## Verification and local artifacts

Compatibility checks cover event reset modes, semaphore counts, wait-any
ordering, atomic wait-all, overlapping object sets, timeout/cancellation cleanup
and 200 back-to-back event handoffs. They pass before and after the optimization.
The portable test was repeated 20 times on Windows and 10 times on Android's
actual libc++ runtime. Eight related host synchronization/thread tests passed.
After review, overlapping-wait tests wait for all three registrations before
signaling, use infinite waits, and cancel via a separate watchdog on failure.
The registration counter is compiled only into the standalone test. The final
version again passed 20 host repetitions and 10 Android repetitions. These are
compatibility checks; the CPU recordings above are the optimization
evidence, rather than a timing-sensitive unit-test threshold.

Local ignored evidence in the `camera-debug` checkout:

- `out/handheld-042/android-336-first/`: original user log/config and API 28 profile.
- `out/handheld-042/android-api28-control/`: unsampled API 28 run.
- `out/handheld-042/android-tls29-{profile,control}/`: isolated native TLS runs.
- `out/handheld-042/android-tls29-targeted-{profile,control}/`: combined candidate.
- `out/handheld-042/android-intro-comparison.json` and
  `android-profile-comparison.json`: extracted measurements.
- `out/build/android-tls29/`: current unstripped binaries; use SDK Ninja and
  the existing current generated diagnostic sources, Release, validation off.

Private APKs are under the main checkout's `out/android-native-tls-20261001/`.
The normal candidate is `FreeRidersRecompiled-0.4.2-native-tls-android10-arm64.apk`.
It carries the same ABI-9 shader pack and bundled 336-entry Vulkan manifest.
Locally learned extra pipeline entries are retained in the device cache.
Signature, 16 KB alignment, minimum SDK and asset/native-library bytes are
verified during packaging. Profileable builds are temporary; restore the
normal APK and the user's original `debug.env` before handing back the device.

Delivery verification: the normal APK was installed successfully; its manifest
contains no `profileable` or `debuggable` attribute and requires API 29.
The original 67-byte `debug.env` was restored and read back with an identical
SHA-256. The launcher opens normally. Final APK SHA-256:
`1a2fa9e295c7feeb90e43872b16a0713410a79d9f27e6a6a888e9f5b6d49b18d`.
The final game's `.text` and `.rodata` sections and all assets match the
measured targeted-wakeup candidate byte for byte. 3D race performance still
needs the user's same-course play test.
