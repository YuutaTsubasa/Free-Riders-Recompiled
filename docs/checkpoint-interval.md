# Guest checkpoint interval

Generated guest code visits a checkpoint at function entries and loop labels.
Calling the execution permit less often reduces host CPU overhead. Windows now
defaults to 256 entries between permit calls; Android, Linux and macOS retain 32.
This integrates knuckleslee's checkpoint-interval contribution from Issue #33.

`SFR_CHECKPOINT_INTERVAL` accepts a complete decimal integer from 1 through 4096.
Unset, empty, out-of-range or malformed values use the platform default. Use 32
to compare the earlier Windows behavior. The runtime logs the effective value
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
comparisons were inconclusive, which is why its default remains unchanged.

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
