# Guest checkpoint interval

Generated guest code visits a checkpoint at function entries and loop labels.
Output of the current generator (or of `scripts/loop_head_checkpoints.py`,
which the build applies to older output unless `SFR_LOOP_HEAD_CHECKPOINTS` is
off) checkpoints only labels a loop returns to, so the same interval spans more
guest work than with checkpoints at every label (performance.md, 2026-10-20:
generated code). Calling the execution permit less often reduces host CPU overhead. Every
platform defaults to 256 entries between permit calls: Windows since Issue #33
(knuckleslee's checkpoint-interval contribution), Android, Linux and macOS since
the AYN Thor measurement below (they kept 32 until then).

`SFR_CHECKPOINT_INTERVAL` accepts a complete decimal integer from 1 through 4096.
Unset, empty, out-of-range or malformed values use the default. Use 32 to
compare the earlier behavior. The runtime logs the effective value
once as `GUEST_CHECKPOINT interval=...`.

Cancellation and urgent waiters are checked at each permit call, while the
ordinary scheduling quantum clock is sampled every 64 calls. An interval is an
entry-count bound, not a millisecond guarantee. Existing reservation handling
and race/UI time remain unchanged.

## Evidence and limits

The reporter's original measurements used a v0.4.5 fork. Separate current-runtime
checks on October 2 used the v0.4.6-based runtime with strict interval selection,
isolated saves/caches, audio enabled, complete drawing and normal elapsed game
time. Uncapped stationary races used the same executable and approximately
matched draw counts. For the predefined HUD 45–80 second window:

| Backend | Interval order | FPS by run | Candidate versus adjacent 32 baselines |
| --- | --- | --- | --- |
| D3D12 | 32 / 256 / 32 | 86.923 / 90.162 / 87.380 | +3.73% / +3.18% |
| D3D12 | 256 / 32 / 256 | 92.877 / 87.278 / 91.517 | +6.42% / +4.86% |
| Vulkan | 32 / 256 / 32 | 78.474 / 80.585 / 77.257 | +2.69% / +4.31% |

Both other predefined time windows were positive. These are results from one
i9-14900KF/RTX 4090 host, not a promised gain for every PC or course. Android
comparisons by frame rate were inconclusive at the time.

On an AYN Thor (Snapdragon 8 Gen 2, Android) the permit call was measured by
its main-thread work instead (HOST_THREADS, Dolphin Resort moving race, frames
13000-21000, same APK with `SFR_CHECKPOINT_INTERVAL` set, two runs each): with
32, `guest_checkpoint_permit` and the lease check were about 3% of the main
thread (simpleperf), each call reading several thread_locals, which in Android's
dynamically loaded libmain are calls to the linker's TLS resolver.

| Interval | drawn_ms | undrawn_ms | frames skipped of 9600 |
| --- | --- | --- | --- |
| 32 | 16.02 / 16.11 | 9.31 / 9.99 | 111 / 203 |
| 256 | 15.76 / 15.85 | 9.20 / 8.41 | 65 / 39 |

Separate instrumented 32/256 pairs on both Windows backends delivered all 18
scripted left/right/release transitions to both original consumers. Each run's
121 audio-health samples showed no engine-glitch increments, empty/full queue
events, failed submissions or nonfinite samples. These checks do not establish
physical controller/display latency, audible quality, or improved responsiveness.
They are not used as throughput measurements.

The entry-state regression uses real execution leases to cover mixed function
entries/loop checkpoints, cancellation and urgent handoff at the next permit
boundary, including with an unexpired one-hour ordinary scheduling quantum.
The existing execution scheduler tests cover reservation and scheduling rules.
Private raw game captures and source/save hashes remain outside the repository.
