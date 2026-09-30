# Handheld performance candidate — 2026-09-30

Based on v0.4.2 (`ddd9e6e`). This is a private test candidate, not a published
release. It preserves the 60 FPS limit and guest game clock.

## Changes

1. Guest suspension waits use notification instead of repeatedly sleeping for
   1 ms. A stop-aware condition variable retains early resume, nested counts,
   cancellation and shutdown semantics. `SFR_SUSPEND_NOTIFY=0` restores polling
   for a comparison with the same binary.
2. With import tracing disabled, synchronous/asynchronous file reads no longer
   hash their buffers for logging. Protection queries no longer populate a
   diagnostic map, and allocation/free logging no longer scans memory statistics.
   Read completion still publishes data/status before signaling its event.
3. Graphics pipeline identity excludes blend/stencil padding and includes the
   consumed Vulkan input location. Regressions demonstrate both defects. This
   is not proof that padding caused the Ally's observed three-second stall.
4. Opt-in main-thread wait and movie telemetry separates native waits from
   permit reacquisition. Windows timer-resolution calibration is available but
   the timer request stays disabled by default.

## Evidence

The supplied Ally X v0.4.2 capture used Vulkan, 720p rendering and Radeon 890M.
Its late window ran about 32 FPS, with 18.9 ms of main-thread blocking per frame.
An isolated large stall spent 2,989.54 ms creating four pipelines. Those are
separate costs; the capture does not identify the main wait's producer.

A desktop pilot attributed the largest native wait to `NtWaitForSingleObject`,
caller `0x824d2868`, target `0x7210001c`; producer hints identify a worker that
self-suspends. Its original host waiter polls at 1 ms. The producer hints are
correlation, not a timestamp proving which signal satisfied the wait.

Directly timing the real `GuestThreads` resume-to-waiter-return path produced:

| Mode | Run 1 mean | Run 2 mean | Samples per run |
| --- | ---: | ---: | ---: |
| Polling | 1,227.36 µs | 1,242.06 µs | 128 |
| Notification | 5.19 µs | 5.40 µs | 128 |

The benchmark lets the waiter park before resuming, alternates separate
processes, and excludes guest permit reacquisition and subsequent guest work.
It establishes reduced wake latency on this desktop; it is **not a game FPS
ratio or a measurement from either handheld**.

The first desktop boot-to-race comparisons overlapped another worktree's game
process. They remain useful functional captures but are excluded from FPS
improvement claims. Later runs must reject overlapping game processes.

Four subsequent runs used the same candidate executable, Vulkan at 720p/60 FPS,
audio on, the same scripted route/save seed and pipeline-cache seed, with no
overlapping game process. Each reached 11,500 presents (about 40 seconds into
the race), then stopped at its intentional present limit. The late frame window
was 9,500–11,000; wait aggregates used report boundaries 9,300–11,300.

| Run order / mode | Mean FPS | p95 frame ms | Draws/frame | Primary native wait ms/call |
| --- | ---: | ---: | ---: | ---: |
| 1 / polling | 56.42 | 20.95 | 837.1 | 1.900 |
| 2 / notification | 54.24 | 21.30 | 876.7 | 1.465 |
| 3 / notification | 56.58 | 20.15 | 843.8 | 1.416 |
| 4 / polling | 55.89 | 20.27 | 842.6 | 1.877 |

The work wait consistently falls, but **overall FPS improvement is not
established**. The first notification run is slower and renders more geometry;
the reverse comparison is slightly faster with almost equal draw counts.
These samples do not justify a broad speedup or a guarantee against regression
on another device. Keep the polling comparison available for handheld acceptance.
Screenshots confirm the same course/HUD and no obvious new visual regression
against the published v0.4.2 reference; AI/item/position differences remain.

The distributed Windows runtime SHA-256 is
`ba08292dd31d9a1ebd1ba1f48bc07e0c8cec4259691048a0e1ec6a91c489dfb7`.
No wait-collector windows saturated in these runs. Intro runs with import
tracing off/on also reached their intentional limits. The disabled diagnostic
paths emitted zero asset-read hash, protection-map or allocation/free-statistic
lines; enabled import tracing still emitted them. Small content-save read
messages intentionally remain outside this change.

## Intro and remaining uncertainty

The historical 2 FPS decoder report predates the per-core scheduler and is not
evidence for the current build. Present count can include unchanged movie
frames; `MOVIE_TRACE success_returns` is not an independently verified decoded
frame count. The candidate preserves the existing blocking movie compatibility
patch. Slow video with normal music on the handhelds still needs a fresh movie
capture to distinguish decoder throughput from scheduling and presentation.

## Validation

- Windows CTest: 118/118 passed.
- Linux CTest: 112/112 passed.
- Python: 266 tests completed, 11 skipped, no failures after retrying with
  access to the normal temporary directory.
- Android ARM64 Release build succeeded. APK signature matches the installed
  v0.4.2 certificate; 16 KiB alignment and bundled ABI 9 shader pack validated.
- Independent review found no blocking synchronization or diagnostic-guard
  issues. Capture scripts now clear inherited wait-result logging too.

Package hashes and gameplay acceptance are recorded in the local delivery report.
Ally X is unavailable locally and Pocket S2 Pro disconnected during the work.
Neither handheld's improvement is yet verified. Its original Android
`debug.env` backup remains in `out/handheld-042/debug.env`; the earlier metrics
flag must be restored when the device reconnects.

See [wait tracing](wait-tracing.md) for capture fields and interpretation.

## Xenia acceptance baseline

The user's performance target is to outperform Xenia on the same device,
without changing game speed or reducing comparable visual quality. This is a
target, not a measured result: no paired Xenia measurements are available yet.
The existing desktop results above cannot establish that target has been met.

For a reproducible comparison:

- Record the exact Xenia fork, commit/build, configuration and any input or
  compatibility patches needed to play Sonic Free Riders. Confirm that both
  builds reach the same course and preserve normal game timing.
- Start with the Ally X: use the same power mode/TDP, power connection, driver,
  internal resolution (initially 1280x720), output resolution, audio and scene.
  Disable Camera Input and custom VRM for the engine baseline. Record each
  renderer and any visual differences; do not assume matching option names
  produce matching work or image quality.
- Run the same Intro and race route at least three times per build, alternating
  order, without concurrent game processes. Separate cold shader-cache results
  from warmed-cache results and allow comparable thermal conditions.
- Compare normal-speed playback and audio/video alignment first, then sustained
  FPS, p95/p99 frame times and long stalls. Present rate alone cannot establish
  movie playback speed. When both reach the 60 FPS cap, report a tie in FPS and
  compare frame-time consistency and measured resource/power costs; do not
  uncap a fixed-step game to manufacture a speedup.
- Claim faster performance only when repeatable paired results exceed run-to-run
  variation. Keep published v0.4.2 as a third baseline to detect regressions.
  Desktop results do not substitute for handheld acceptance, and a Windows
  Xenia result does not establish an equivalent Android comparison.

A targeted local search found a directory named `XeniaProject`, but it contains
recompilation project sources; no Xenia executable/configuration was found there.
Selecting and validating a runnable Xenia baseline remains outstanding.

## Producer-side follow-up and Claude coordination

Claude's separate `.worktrees/claude-handheld-perf` checkout investigates
Windows timer policy and high-resolution timed waits. Codex has not modified
that checkout. A shared `out/CODEX-PERFORMANCE-HANDOFF.md` in the primary
repository records locations, evidence and integration cautions; receipt by
Claude has not been confirmed.

An isolated desktop probe set and read back default, ignored and explicitly
honored timer-resolution policies in separate processes, then reversed order.
Across 96 samples per kind per process, ordinary 1 ms and 100 us sleeps averaged
about 1.45–1.53 ms in all policies. High-resolution waitable timers averaged
about 1.50–1.52 ms for 1 ms requests and 0.498–0.505 ms for 100 us requests.
The probe balanced timeBeginPeriod/timeEndPeriod and changed only its own
process. This supports investigating submillisecond timed waits but does not
reproduce the Ally's suspected 15.625 ms quantization or establish its cause.
The Ally's late 6,300–7,418 window has zero reported frame-cap pacing time;
changing the cap alone therefore cannot explain that race bottleneck.

Selected-worker diagnostics (`SFR_WAIT_TRACE_GUEST=7`) were added without
changing scheduling. `worker7-notify-1` reached its intentional 11,500-present
limit, with no overlapping game process, 11,500 complete present rows and no
collector overflow. Seven late worker windows cover 35.04 seconds: 32.18 seconds
are suspension awaiting the next frame, 0.220 seconds are suspension permit
reacquisition, and about 0.099 seconds are native critical waits. No short
sleep/poll sites were reported. The remaining 2.50 seconds includes execution,
preemption and logging, not independently measured pure CPU time. Waits are
charged at completion, so boundary-crossing waits can skew window complements.
Main-thread and worker time overlap and must not be added together.

A second run, `worker7-cpu-profile-1`, sampled worker 7's host instruction
pointer after frame 9,500. It also reached 11,500 presents without overlap or
dropped wait rows. Of 23,188 samples, 92.73% were outside the executable's code
(unresolved OS waits/libraries). Leading executable symbols were
`sub_8280F5F8` (158 samples), `sub_8280E170` (92), `sub_8280CB80` (68), and
`sub_82814758` (68). These are nearest-preceding linker-symbol attributions, not
call stacks. They identify guest execution to investigate rather than a hot
short-sleep loop. They do not yet justify replacing guest algorithms or claiming
a frame-rate improvement. Broader main-thread CPU attribution is still needed.

Both diagnostic runs used executable SHA-256
`f49e3253b0e14202ed7a6d3825ac39d726f9c56ddb1a927c20f0df2968055a3d`.
Raw probe/capture/analysis files are under `out/handheld-042` in Codex's worktree.
This follow-up is not repackaged into the previously delivered test ZIP/APK.
