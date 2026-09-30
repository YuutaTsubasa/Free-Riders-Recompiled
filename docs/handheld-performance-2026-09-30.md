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

## Main-thread profile and vertex conversion follow-up

`main-cpu-profile-1` profiled guest 1 after present 9,500 with the same f49e3253
executable. It reached the intentional 11,500-present limit, no overlapping game,
and no wait overflow. Of 23,006 instruction samples, 32.22% were outside the
executable's code. Leading executable symbols included `swap_words_into`
(1,007 samples, 4.38%), `native_draw` (749, 3.26%), `memcpy_repmovs` (747, 3.25%)
and `MoveAbove15` (706, 3.07%). These are sample fractions, not CPU-only fractions
or frame-time speedup estimates. The executable and matching map were preserved
under `out/handheld-042/swap-baseline` before further changes.

Disassembly showed the byte-swap loop reloading both span pointers after each
16-byte store. Caching `.data()` in local pointers eliminates those repeated
loads. The conversion still uses the same SSSE3/NEON/scalar paths, byte bounds,
tail semantics and output format; it adds no instruction-set requirement.
Characterization covers independent source/destination offsets 0–15, lengths
0–80, unequal spans, scalar-reference output, immutable source and destination
guard bytes. It passed before and after the optimization. The Windows runtime
built, and native_formats, guest_graphics and native_pipeline_key passed (3/3).

An isolated microbenchmark compared the compiled original function with the
pointer variant in old/new/new/old order, using 32 KiB, 1 MiB and 8 MiB buffers,
aligned/one-byte-offset pointers, and ordinary/write-combined destination
memory. Each sample transferred 128 MiB repeatedly from the same warm source.
Across those cases the two-sample mean conversion time was 4–21% lower. Samples
lasted only 2–5 ms and are noisy; this establishes neither a game FPS percentage
nor a handheld speedup. The raw source, outputs and disassembly are saved in
`out/handheld-042/swap-*`.

`swap-original-1` completed a race baseline at 11,500 presents, with no overlap.
The modified run `swap-optimized-1` was intentionally aborted at present 7,843
because another game started in Claude's checkout (PID 47808). That run is
excluded from performance comparison. The modified executable SHA-256 is
`f229f0564f0e6ea665bd7ca65c2b2a467e38297fbcb128b86ae8654dfdf6df08`.
Full-race validation and an uncontended original/modified comparison remain
pending while Claude runs its own priority experiments. The published release
and previously delivered private candidate have not been replaced.

For the future Xenia baseline, the fork maintainer's
[netplay compatibility list](https://github.com/AdrianCassar/xenia-canary/wiki/Netplay-Compatibility)
lists Sonic Free Riders with the
[No Kinect Patch](https://gamebanana.com/mods/456720). This is a candidate route
to a playable comparison, not a measured performance result. A specific build,
patch version and matching scene/quality settings still need to be validated.

### Completed modified race capture and Xenia source review

`swap-optimized-2` completed the scripted 11,500-present capture (intentional
present-limit exit 3), zero overlap, 11,500 complete rows and zero dropped waits.
This covers the opening race segment, not a finished lap. The frame-11,250
screenshot was inspected alongside the original; both render the track,
character and HUD, with different race/AI states. This is a visual smoke check,
not a pixel-identical replay or complete visual regression test.

| Capture | Mean ms | p95 ms | p99 ms | Mean draws | Draw ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| swap-original-1 | 17.9204 | 20.3538 | 21.5929 | 831.81 | 5.3945 |
| swap-optimized-2 | 17.6087 | 19.9010 | 21.3398 | 843.54 | 5.5519 |

Window: frames 9,500–11,000 inclusive, 1,501 samples per capture. Frame times
improved in this pair but drawing cost did not, and race workloads differ.
One pair does not separate an optimization effect from run-to-run variation.
Do not present it as a proven FPS gain. The native_formats, guest_graphics and
native_pipeline_key tests were rerun after capture: 3/3 passed.

The user's requested [Xenia source comparison](xenia-performance-comparison-2026-09-30.md)
pins upstream and Canary revisions, identifies GPU-side vertex conversion and
audio-pump differences, and corrects the assumption that all Xenia variants
map a guest priority of 16 above normal. Source review and this desktop capture
do not resolve the handheld A/V symptom or establish a win over Xenia.

### Vertex cost decomposition

Opt-in `SFR_FRAME_METRICS` now separates cached source bytes, swapped source
bytes, DEC3N expanded output bytes, and swap/repack timings. Disabled metrics
take no added clock readings. Source totals reset at the existing present
boundary. Repack output is a different representation and is not added to the
source-byte total.

`vertex-costs-1` completed 11,500 presents, intentional exit 3, zero competing
games and zero dropped waits. The verifier first rejected the previous runtime's
log for missing metrics; it then validated all 11,500 new rows, including
`vertex_cached_bytes + vertex_swap_bytes == vertex_bytes`. Runtime SHA-256:
`0f3bd81b2fe1d6c05aae44af5135a2b2c2bcbd69ec096643340ceb148c3d095f`.
Late-window means (9,500–11,000): 33,908,089 source bytes, 29,274,821 cached,
4,633,268 swapped, 3,052,335 expanded output bytes; swap 0.6610 ms, repack
0.7049 ms, total measured draw 5.0680 ms. Added clocks/log fields have observation
cost, so this is not an FPS comparison against the previous executable.

These measurements justify evaluating a fused CPU endian/DEC3N conversion to
avoid the intermediate scratch copy before considering a shader ABI change.
No conversion behavior changed in this diagnostic step. Raw capture and
`vertex-analysis.json` are under `out/handheld-042/vertex-costs-1`.

### Fused conversion experiment: not retained

The fused helper removed the intermediate swapped copy and passed byte/bounds
tests, independent review and Android ARM64 compilation. Actual game layouts
exposed a 28-byte / three-attribute case that the first synthetic benchmark had
missed. Specializing the observed strides recovered that microbenchmark, but
did not establish a stable game benefit.

All rows below use frames 9,500–11,000 inclusive of separate 11,500-present
captures. Each finished with intentional present-limit exit 3, no competing
game, zero dropped waits, and valid cached+swapped=source byte accounting.

| Capture | Mean frame ms | p95 ms | p99 ms | Mean draws | Swap+repack ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| vertex-costs-1 (original) | 17.5328 | 20.3751 | 21.7096 | 851.25 | 1.3659 |
| vertex-fused-1 | 17.6701 | 20.1186 | 21.0903 | 855.87 | 1.5503 |
| vertex-fused-layouts-1 | 17.9665 | 20.2794 | 21.2500 | 847.02 | 1.6043 |
| vertex-fused-actual-1 | 18.0601 | 21.0447 | 22.1131 | 861.74 | 1.5966 |
| vertex-control-repeat-1 (original) | 17.9009 | 21.1285 | 22.2190 | 857.47 | 1.6630 |

The original executable itself drifted from 1.366 to 1.663 ms conversion time.
The final fused candidate falls inside that variation. This does not prove
either an improvement or a pure regression; no FPS benefit is claimed.
Source conversion volume stayed close to 4.63 MB/frame. The archived candidate
SHA-256 is `7ba93d7acf05f49a847eefaf8f5c765394992f3df9594b5ea26204f0853904e7`.

The four experiment source/test files were restored to commit `50e58c5` after
saving a reapplicable patch and exact executable/map under
`out/handheld-042/fused-dec3n-experiment`. `git apply --check` accepts that patch.
The restored runtime builds and native_formats, guest_graphics and
native_pipeline_key pass (3/3). Opt-in vertex-cost metrics remain committed.
Neither the delivered candidate ZIP/APK nor Claude's files were changed.

### Intro coverage gap

Read-only ffprobe metadata reports `titleMovie_j.wmv` as WMV3, 1280x720,
30000/1001 fps, with WMA Pro audio at 48 kHz and duration 72.414 seconds.
The earlier `intro-final` capture lasts only 31.969 seconds and contains one
30-call movie trace window. That proves neither full playback nor A/V sync.
Movie draws stop around present 671 and the title menu appears; the trace
does not yet distinguish the game's own state change from player completion.
Extend diagnostics and capture coverage before changing decoder/audio timing.

### Intro test cancellation isolated

The extended opt-in movie trace records the first call, return-status changes,
cumulative calls/success returns, elapsed host time, caller and incoming flags.
It does not change arguments, readiness waits or skipping. The old-log verifier
failed for missing coverage fields before the edit; new captures pass it.

`intro-coverage-long-1` ran 7,200 presents / 124.703 seconds with the previous
test environment. Four short movie episodes ended after about 1.6 seconds.
`intro-flow-1` enabled entry diagnostics (not a performance comparison): the
game calls `sub_82817C40` from `8243AA44`, setting player stop flags before
the subsequent `0x16660026` return. It is not natural exhaustion of the file.

Cause of the test cancellation: the race scenario's inherited
`SFR_NUI_HAND_CENTRED=1` presents an already raised/centred virtual hand. Setting
only that interaction flag to `0` in `intro-resting-hand-1` sustains playback to
completion: 2,176 player calls, 2,175 success returns, terminal status at present
2,801, 72.4824 seconds since the first call began. Source duration is 72.414
seconds. Intermediate screenshots show changing video content. The full run
finished 4,000 presents / 104.046 seconds, exit 3, no overlap or dropped waits.
The private runner now uses resting hands by default for Intro.

This corrects the test harness, not the handheld slowdown. Success-return count
differs from the file's 2,169 video packets, so do not call it a decoded frame
counter. Matching overall playback duration on this desktop also does not prove
per-frame A/V sync or accepted/played audio samples. Earlier short Intro captures
must not be used to claim full movie validation. Runtime SHA-256:
`5a211a10094847bcd4ff0b971da4f7735ac644c872c5850e317b1e92521e84ba`.

The diagnostic runtime builds; wait_trace, native_formats, guest_graphics and
native_pipeline_key pass (4/4). Movie trace accounting passes for the long,
flow and corrected full-playback captures. No shipped package changed.

### Movie decoder: exact-range partial vector loads

With resting hands, `intro-decoder-profile-1` samples guest 32 (worker
`828265B0`, the movie decoder on guest processor 1), starting after present 700
and ending at present 1,600. The sample contains 19,706 observations; 44.85%
are outside executable code, including unresolved OS/library/wait samples.
The checked partial-vector left/right helpers and their runtime wrappers
account for 2,277 samples (11.55%). Unlike full-vector loads, the partial helpers
preflight and then redo the page lookup for every byte.

The small `vector_memory.cpp` change reuses `GuestMemory::fast_read` for the
exact selected slice. Eligible ordinary pages then use ordered volatile byte
reads. Left loads never read their excluded prefix; right loads never read
their excluded suffix, and an aligned right load still touches no memory.
Special pages retain the previous guard/provider checks and per-byte callback
semantics. No scheduling, game clock, codec math or generated code changes.

The private `vector-load-bench.cpp` compares every offset across a whole page
against independent copies of the original helpers, then alternates timing
order over four repeats of two million calls each. The pre-edit ratio was
1.002 for left loads and failed the experimental <0.7 criterion (exit 3).
After the change, median current/original ratios are 0.4072 (left) and 0.4917
(right), with matching checksums. These are warm helper timings, not game FPS.

The same Intro profile after the edit (`intro-decoder-profile-fast-1`) contains
19,616 samples; partial helpers plus wrappers account for 1,708 (8.71%).
Outside-executable samples rise to 50.01%, which cannot all be called idle time
without stack resolution. Both profiles play near the video's normal 30 Hz.
They finish 1,600 presents with no overlap/dropped waits. Runtime SHA-256:
`0811e711532a6c6837c222cd97d2d429759526cc8065a452bb839a72ecbe81b2`.
The exact baseline executable/map are preserved in
`out/handheld-042/movie-baseline`; analysis checks the executable hash.

Windows guest_memory and vector_memory tests pass (2/2), including all byte
offsets, guards, providers, top-address and reservation cases. Android ARM64
vector-memory test compilation/linking passes; it was not run on a device.
Read-only independent review found no actionable defect. Full post-edit
playback (`intro-resting-hand-fast-1`) finishes 4,000 presents / 104.046 seconds
without the sampling profiler, overlap or dropped waits. The movie reaches its
terminal status after 72.476 seconds, consistent with the original 72.4824
seconds and source 72.414 seconds. The mid-movie screenshot shows the same
segment rendering correctly (not an exact-frame visual regression comparison).
This reduces measured helper work while preserving playback duration on the
desktop; it is not proof of a handheld FPS gain. Race smoke validation and
an updated distributable candidate remain pending.
