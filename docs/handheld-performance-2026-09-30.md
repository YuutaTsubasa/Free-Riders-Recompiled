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
desktop; it is not proof of a handheld FPS gain.

### Refreshed private candidate and race validation

`vector-load-race-1` completes 11,500 presents in 195.718 seconds, intentional
present-limit exit 3, with no overlapping game process or dropped wait events.
The screenshot at present 11,250 shows the rider, track and HUD in the race.
This is a roughly 40-second race segment after menu/loading automation, not a
completed lap. The late 1,501-frame window averages 17.917 ms, p95 20.095 ms,
and p99 21.334 ms. It is a desktop smoke test, not an isolated speedup result.
All 11,500 frames satisfy cached bytes + swapped bytes = source vertex bytes.
Late means are 34.43 MB source, 29.79 MB cached, 4.64 MB swapped, with 0.787 ms
swapping and 0.817 ms repacking. Repacked output bytes use a different unit of
accounting and must not be added to source bytes.

Windows runtime/launcher and Android ARM64 runtime/launcher/SDL builds succeed.
Windows CTest passes 118/118. A subsequent complete build relinks nine shader
and render executables; the affected test selection then passes 10/10.
Android compilation retains existing generated-code FPCR operand warnings;
Android tests and the application have not been run on a handheld.

The second private package is based on source commit `98d6534`, named
`0.4.2-handheld-test2-20260930`, in the primary checkout's
`out/handheld-vector-candidate-20260930`. It includes the retained pointer and
partial-vector improvements and new opt-in tracing. The earlier candidate in
`out/handheld-wait-candidate-20260930` remains intact. Android keeps version
0.4.2/code 12 and the same signing certificate, shader pack and pose-asset scope
as the official APK. Windows retains camera models and runtime dependencies.
No game data, personal settings or save files are included.

Claude's separate timing/priority commits `eafeba9` and `567ab89` have been
reviewed read-only, not integrated into this candidate. A claimed handheld
coarse-timer cause still requires measurements on that device. No Xenia runtime
comparison or handheld acceptance has occurred; the next useful comparison is
this package versus v0.4.2 on the same powered device, settings and scene.

### Remaining main-thread work and constant upload probe

Fresh `main-cpu-profile-current-1` samples guest 1 after present 9,500 with the
test2 runtime (`0811e711...`). There are 22,534 samples, 35.10% unresolved outside
the executable. `swap_words_into` accounts for 2.39% and `load_vector_left`
0.76%, versus 4.38% and 1.56% in the earlier profile. These sampling differences
are consistent with the isolated helper improvements, but scene timing/draw
counts differ and they are not an exact end-to-end speedup measurement.
The current late race metrics show index decoding at 0.215 ms, while native
command recording/upload work costs about 2.49 ms per frame. Avoid investing
in a new index cache based only on the million indices processed per frame.

`SFR_CONSTANT_REUSE_TRACE=1` instruments the two 4 KiB constant uploads per draw.
It keeps CPU-only prior-byte snapshots and invalidates both at every upload-ring
flush. It does not skip uploads or change bindings. Disabled mode allocates no
shadow storage and makes no byte comparisons. The verifier correctly rejects
the older capture without these fields before checking the probe capture.

`constant-upload-probe-1` completes 11,500 presents / 195.157 seconds without
overlap, dropped waits or a crash (intentional present-limit exit 3). All frames
satisfy upload bytes = draw count * 8192, reusable bytes <= upload bytes minus
the first draw's two buffers, and whole-buffer accounting. In the late
1,501-frame window, the mean is 860.85 draws, 7,052,117 uploaded bytes and
4,342,674 reusable bytes per frame: **61.58% of constant upload bytes repeat**.
Runtime SHA-256:
`3f5f4acbecc4e21ceb4d316377add20ced431acb215187c368cf7ec941c4ba95`.
The present-11,250 image shows the race, rider and HUD. Windows graphics,
resolution and guest-graphics tests pass (3/3) with the probe disabled.
The Android ARM64 runtime also builds successfully; no device run is implied.
The exact probe executable and map are archived under
`out/handheld-042/constant-probe-baseline` for the next comparison.

This is sufficient evidence to investigate reusing exact constant uploads
within one ring lifetime, not evidence that a cache already saves time. The
probe adds comparison/shadow-copy work, so its frame time must not be compared
as a speedup. The experiment should keep allocation layout/ABI unchanged,
invalidate on every flush, bind the original immutable upload offset on a hit,
and test changed bytes and ring reuse on both Vulkan and D3D12. The distributed
test2 ZIP/APK remain unchanged and do not contain this newer probe.

### Constant upload reuse: correctness and local A/B/A

The opt-in `SFR_CONSTANT_UPLOAD_REUSE=1` experiment retains an exact 4 KiB CPU
snapshot and immutable ring offset independently for VS and PS. Hits bind that
offset instead of copying again; misses write the reserved slot and update the
snapshot. Every flush invalidates both stages, before ring reuse or switching.
The allocation layout, constant buffer sizes, shader ABI and game clocks are
unchanged. Default remains off while handheld performance is unverified.

The helper test first failed against always-upload behavior, then passed with
reuse. It covers initial zeros and offset zero, unchanged bytes, a changed last
word, signed zero and NaN payloads, immutable earlier uploads and explicit reset.
GPU readback covers independent stage changes, identical values at different
draw offsets, synchronous reset, four asynchronous presents through both rings,
and a near-capacity reservation that forces a flush. Both Vulkan and D3D12 pass.
The first D3D12 fixture failed before reuse was enabled: DXIL inspection showed
TEXCOORD at different signature registers because the test PS omitted the VS's
SV_Position input. Retaining that input fixed the fixture; no backend workaround
was added. Private debugger output and linker map localize the old failure to
binding a pipeline after that invalid shader pairing.

Independent read-only review found no actionable defect, including review of
the additional async/capacity tests. The Windows suite with reuse enabled passes
119/119, and focused D3D12 readbacks pass 2/2. Android ARM64 runtime and helper
test compile/link, but no Android execution occurred. A fresh `adb devices -l`
still shows no connected device. Capture script syntax parses and its new
`-ConstantReuse on|off` option restores process environment and records the mode.

Same-executable, no sampling profiler, wait trace disabled, 60 FPS cap, 720p/100%
Vulkan race A/B/A results (1,501 late frames per run):

| Run | Reuse | Frame ms | Record ms | Draws/frame | Record us/draw |
| --- | --- | ---: | ---: | ---: | ---: |
| constant-cache-off-1 | off | 17.8727 | 2.5590 | 868.35 | 2.9469 |
| constant-cache-on-1 | on | 17.5180 | 2.0734 | 873.96 | 2.3724 |
| constant-cache-off-2 | off | 17.6626 | 2.4851 | 845.04 | 2.9407 |

All runs complete 11,500 presents without overlap/crash; exit 3 is the configured
present limit. Enabled mode saves 4,392,328 bytes/frame (61.35% of VS/PS requests).
Every frame's accounting is bounded and whole-buffer aligned; off runs save zero.
The enabled race screenshot shows the track, rider and HUD. Record time per draw
is about 19% lower than either control. Overall frame time is only about 0.8–2%
lower; do not describe this as a 19% FPS gain. Draw counts and scene timing vary,
and integrated/mobile GPUs may have a different copy/compare cost balance.

Benchmark executable SHA-256:
`f71877be8dbb24cb90b54dd8cbc2e45650eb93de8e0340a7a895d75b85eecd62`.
Exact executable/map, patch/new helper sources and comparison JSON are preserved
under `out/handheld-042/constant-cache-experiment`. No public release or existing
private package was changed by this experiment.

The final opt-in build also completes `constant-cache-intro-1`: 4,000 presents
in 103.985 seconds, expected present-limit exit, no overlapping game or dropped
wait records. Movie trace accounting passes and reports completion after
72,476.4 ms, consistent with the prior 72,476 / 72,482.4 ms captures (source
movie approximately 72.414 s). The present-2,000 image contains the Intro movie.
Matching playback duration is a timing regression check, not per-frame A/V sync
validation or proof that handheld video slowdown is fixed. Final Intro runtime
SHA-256: `8bdc26ee7e0333fba9e10ab73749f3fd02907fc38033d8a1a20ed7f872e46e0b`;
the race A/B/A used the separately archived executable identified above.

### Integrating Claude's host timing and priority experiments

The prior turn's constant upload work was committed as `aee1d56`. This turn
imports Claude's `eafeba9` and `567ab89` as `467ccb9` and `852e2a7`, retaining
original authorship, only in Codex's checkout. The suspension conflict keeps
the stop-aware notification path; precise_sleep is used only by the optional
legacy poll. Claude's checkout remains unchanged.

Integration corrections pair the host timer request through the existing
NativeTimerResolution owner, report execution-only versus combined policy
acceptance separately, and test host timing on/off and priority on/off/default
in separate processes. The original tests failed for their documented opt-out
modes because they always expected enabled behavior. Those cases now pass.
Scheduler upper-latency bounds are measurements rather than correctness asserts.
Docs now distinguish the Ally timer hypothesis from evidence and correct the
Canary comparison (pinned Canary leaves guest priority 16 at normal).

Four same-executable serial races complete 11,500 presents each, no overlap or
crash, intentional present-limit exit 3, zero dropped waits. No sampling
profiler or detailed wait trace; Vulkan 720p/100%, 60 FPS/audio, notification
wait on, older timer-resolution switch off, constant reuse/probe off.
Late frames 9,500..11,000 (1,501 frames) give:

| Run | Host timing | Raised priority | Mean ms | P95 ms | P99 ms | Draws/frame |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| timing-combined-off-1 | off | off | 17.9702 | 20.8162 | 21.9287 | 866.04 |
| timing-combined-host-1 | on | off | 17.2980 | 19.5143 | 20.8234 | 834.22 |
| timing-combined-both-1 | on | on | 16.9346 | 18.8396 | 20.2009 | 817.23 |
| timing-combined-off-2 | off | off | 17.2528 | 19.8649 | 21.1967 | 853.83 |

The off controls differ by 0.7174 ms (~4%). Timing-only lies inside that range;
the both-on run also processes fewer draws. These results do not establish a
stable end-to-end gain. All four startup ordinary 1 ms sleep means are
1.35–1.48 ms: this desktop does not reproduce ignored/coarse timer requests.
Both enabled runs report successful execution-speed and timer-policy opt-out.
The both-on race image at present 11,250 shows the track, rider and HUD.

Measured runtime SHA-256:
`11116d470c5bb437301ec3d2b6fa880c12f00a193df9bfd5e4a520edc6e2dacc`.
Exact exe/map, source delta, mode environments and verified comparison JSON are
archived under `out/handheld-042/timing-integration-experiment`.

Priority boosting is therefore opt-in in the final source (`=1`); its new
default test failed before the default changed, then passed. Host timing remains
enabled in this private candidate as a foreground Windows process policy, with
an explicit opt-out for device comparison; this is not a proven FPS fix or a
release-default decision. Android's existing native priority setter only stores
the relative value and does not elevate OS scheduling, so this priority change
must not be described as an Android performance optimization.

Final Windows suite passes 123/123. Android runtime and host timing/guest thread
tests compile/link; no Android execution occurred. Read-only independent review
found no actionable bugs, including the opt-in default and capture-script follow-up.
The script adds HostTiming/Priority modes, restores them on exit, and records
both explicitly; its syntax parses successfully. Existing private packages and
public releases remain unchanged.

Final `timing-combined-intro-1` uses the rebuilt opt-in-priority runtime SHA-256
`7012329882638475252fb198c0ff5fb038fde46726009338d29ab47beb08adb7`.
It completes 4,000 presents in 104.094 seconds, with intended exit 3, no game
overlap and zero dropped waits. Movie trace structure/accounting passes:
completion after 72,475 ms, consistent with earlier ~72.48-second captures.
The present-2,000 screenshot contains the Intro movie. This checks full playback
duration, not frame-by-frame A/V alignment or a fix for the handheld symptom.
No game remains running. Next deliverable is an isolated combined candidate
with clearly labeled mode comparisons, preserving all existing packages.

### Private combined candidate (test3)

`out/handheld-combined-candidate-20260930` in the primary repository now contains
separate Windows and Android artifacts built from runtime source `fc54993`.
Existing test1/test2 packages and public releases were not replaced.

- Windows ZIP: 60,771,895 bytes, SHA-256
  `4dcd34db08ccf7610f9d94fe6ef14b1e9fc834f4ca171a47ee6f103564e61846`.
- Android APK: 162,366,451 bytes, SHA-256
  `80a3b40a373df7c3836af0080d608417e83af3f9c08e036ba6f3763a2366cc06`.
- Android licenses ZIP retains the official v0.4.2 notices, SHA-256
  `de173c75a4bb4cdc4501527a7b09aef91700c350695af83b49de0ac0ac7e2c10`.

The Windows archive has 39 unique entries; CRC checks, exact runtime/launcher
identity, shader pack identity, Camera model/runtime and D3D12 DLL presence pass.
No game assets, personal settings, save or custom VRM were included. The package
was extracted into a fresh directory and the game run from that directory,
using its own shader pack/tools. `combined-package-race-1` completes 11,500
presents in 195.234 s, intended exit 3, no overlap/dropped waits. Its image at
present 11,250 shows race/character/HUD. This is a packaging smoke test, not a
full lap or a new performance comparison; the runtime is the same 70123298...
binary used by the full Intro validation above.

APK signature verification passes with the existing certificate, version 0.4.2
(code 12), and 16 KB ZIP alignment passes. Native library set, pose asset scope
and shader pack match the official Android baseline; packaged libmain matches
the freshly stripped build. No device is connected, so no install/device test
was performed. Host timing policy does not apply to Android.

How-to-test.txt and five Windows launch scripts explain the default capture,
trace-off capture, host-policy-off control, constant reuse and priority modes.
Build-report.json, Evidence and SHA256SUMS.txt record exact scope and results.
Handheld acceptance and same-hardware Xenia comparison remain unverified.
