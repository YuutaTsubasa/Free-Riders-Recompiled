# Renderer optimization references for AYN Thor

This is source analysis against the current `fcdd8e6` SFR runtime and the
existing Thor captures. It is not a new performance result, an implementation,
or a benchmark demonstrating that SFR is faster than another project.

## Reproducible sources

- UnleashedRecomp: local clean GPU source at
  `5e8695a157ce9d2a783944d63439cc8c76a38fc2`, origin hedge-dev/UnleashedRecomp.
  [video.cpp](https://github.com/hedge-dev/UnleashedRecomp/blob/5e8695a157ce9d2a783944d63439cc8c76a38fc2/UnleashedRecomp/gpu/video.cpp).
- MarathonRecomp: official upstream snapshot
  `04431570d1ad56eaf3dda9ddd9f0efc628c455f5`.
  [video.cpp](https://github.com/sonicnext-dev/MarathonRecomp/blob/04431570d1ad56eaf3dda9ddd9f0efc628c455f5/MarathonRecomp/gpu/video.cpp).
  The local YuutaTsubasa fork was inspected first; conclusions below were
  checked against the upstream GPU file rather than assuming the fork matches.
- Xenia upstream: `95a5c3ee250f80c3b9d139658649d9ffb6db3eec`, the pinned
  revision from the earlier investigation, not a claim about every Canary fork.
  [Vulkan shared memory](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/gpu/vulkan/vulkan_shared_memory.cc),
  [texture cache](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/gpu/vulkan/vulkan_texture_cache.cc),
  [command processor](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/gpu/vulkan/vulkan_command_processor.cc).

Downloaded source snapshots and SHA-256 manifests are under
`out/thor-race-perf/reference-renderers`. No reference implementation was
copied into runtime code.

## Observations and applicability

### 1. Submission lifetime instead of a four-upload rotation: first priority

Unleashed `ProcUnlockTextureRect` (line 2190) and Marathon's equivalent
(line 2527) allocate staging space from the current frame's upload allocator
and record copies into that frame's command list. Unleashed's allocator
(line 465) grows in mapped 16 MiB buffers; resetting it occurs after the
corresponding frame fence wait (lines 2832-2847). Both projects also retain
temporary resources until the relevant frame is finished.

Xenia's `VulkanSharedMemory::UploadRanges` (line 374) appends copies to a
deferred command buffer, grouping regions using the same staging buffer.
Upload allocations carry the current submission index. Completion processing
reclaims command pools/resources by completed submission, and the shared
memory upload pool uses that same completion boundary (line 216).

SFR currently submits every texture copy individually. Four command-list
slots rotate, and a slot must finish before reuse. The Thor phase probe
measured 33.9/18.0/12.1 ms waits at the fifth upload in three frames.
The separate byte verifier saw one frame upload 342 textures / 28,483,584
guest source bytes; these bytes are not the padded staging footprint.

Recommended experiment: accumulate copies in a separate upload command list
and submit it on the existing graphics queue before its consuming draw list.
Retain staging and command-list resources until that submission completes.
Drain pending uploads at every present, explicit flush, readback and shutdown
boundary, not only once per visual frame. Preserve the current copy-before-
draw order and immutable texture versions on invalidation. Bound outstanding
staging by actual padded allocation bytes, with a safe submit/wait fallback
under memory pressure. Measure that budget before choosing its default.

This is preferable to simply enlarging the four-slot ring: a larger ring
still submits once per texture and can only move the eventual wait. Directly
inserting each copy between draws is another option, but SFR's Plume Vulkan
`copyTextureRegion` ends the active render pass (line 3184); repeated pass
breaks are an additional mobile-GPU cost to measure. A separate batch avoids
introducing those breaks within the draw list.

Neither reference recomp is universally asynchronous: both have
`ExecuteCopyCommandList` helpers that submit and immediately wait (Unleashed
line 1330, Marathon line 1641), including resource-loading call sites.
Copying those paths wholesale would not establish an improvement.

### 2. Reduce repeated draw preparation: second priority for sustained FPS

Both recomp renderers use dirty state to avoid re-uploading unchanged shader
constants; Marathon's VS/PS/shared checks are at lines 5335-5351. Both also
have a render-command queue and a dedicated consumer thread (Unleashed line
5249, Marathon line 6106), separating guest-side command production from host
render-command recording.

SFR already has vertex caching and an opt-in exact constant-upload reuse
experiment. It must not be presented as a missing cache to add again. Review
`handheld-performance-2026-09-30.md` before repeating that experiment: desktop
results exist, but they do not prove a Thor win. A stronger dirty-generation
approach could avoid both repeated copies and repeated 4 KiB comparisons,
provided every mutation and flush invalidates the appropriate stage.

First measure constant preparation, vertex/index conversion, command recording
and synchronization on matched race segments. Only then consider separating
the host rendering thread: queued commands must own immutable snapshots,
and guest memory/resource lifetimes must survive consumer lag. Merely adding
a thread around references into mutable guest memory is unsafe.

### 3. GPU conversion: promising but lower priority on this device

Xenia's Vulkan texture cache loads guest-format data through compute dispatch
(line 1450), then barriers and copies the scratch result into the image
(lines 1458-1484). Its shared-memory path uploads changed page ranges, while
the earlier Xenia investigation covers GPU vertex endian/unpack work.

This can move SFR's untile/endian or vertex conversion CPU work onto the GPU.
It is not free: compute, scratch storage, barriers and additional bandwidth
can compete with rendering on Thor. Current CPU texture conversion has already
fallen from about 106 to 50 ms total across the complete verifier fixture.
The separately measured upload waits therefore deserve attention before
replacing those conversions with shaders. Keep a CPU fallback and compare
GPU readback byte-for-byte for each format if this experiment is pursued.

## Validation for the first experiment

1. Instrument upload submission count, actual staging high-water bytes,
   fence wait time, and render-pass breaks. Distinguish texture upload waits
   from the existing presentation-only GPU-wait metric.
2. GPU readback tests: multiple textures, more than four uploads, updates to
   the same guest range, earlier draws retaining old contents, asynchronous
   frame reuse, mid-frame flush/readback and bounded-memory fallback. Validate
   Vulkan and D3D12; do not change texture bytes or rendering order.
3. A/B/A Thor runs with the same scene/checkpoints, resolution, power/thermal
   conditions and diagnostic settings. Measure p95/p99 frame time, frames
   above 50/100 ms, Loading duration, steady-race throughput and staging memory.
   Presented frame indices alone are not equivalent-scene checkpoints.
4. Verify audio, race clock and jump-rating duration. A higher present count
   obtained by speeding game time or dropping required draws is not a win.
5. Ship the experiment only after a normal APK run; remove fixture overrides
   and preserve user saves/settings.

The normal texture-optimization capture still has a 181.634 ms frame, of
which 62.361 ms is texture processing; other >100 ms frames contain little
texture work. Upload batching targets a measured bottleneck, not every cause
of stutter or the entirety of sustained low FPS.

## Friend's Z13 report received after the source comparison

The supplied report.md and report.zip identify an ASUS ROG Flow Z13-KJP,
Ryzen AI Max+ 395 / Radeon 8060S, plugged in under Turbo. The user initially
mentioned i5-3470, then clarified that this may be the friend's other device.
Treat these results as Z13 data; they establish nothing about i5-3470.

The ZIP contains the identical report, runs.csv and eleven frame CSVs, each
with 3,468 rows. It contains no original logs, benchmark script, executable
revision or implementation of the friend's experimental render thread. The
report describes a fork of v0.4.3, without audio, movies or Kinect, running
uncapped with a custom render thread enabled in its baseline. This is not the
current SFR baseline or the Thor workload.

Independent recomputation from the supplied rounded per-frame milliseconds
reproduces every reported run FPS as `1000 / mean(frame_ms)`, to three decimal
places. Median run FPS by configuration is:

| Configuration | Runs | Median run FPS | Median of run-average draws/frame |
| --- | ---: | ---: | ---: |
| baseline | 4 | 95.659 | 639.30 |
| no-render-thread | 3 | 95.746 | 631.30 |
| no-suspend-notify | 4 | 86.755 | 710.55 |

Implications:

- The render-thread experiment shows no consistent gain here; only two rounds
  pair with baseline, one faster and one slower. Keep render-thread extraction
  lower priority than the independently measured Thor upload waits. This does
  not establish that a render thread is useless on every CPU.
- Polling instead of suspend notifications is associated with about 9% lower
  median run FPS. Keep the current notification default. Do not attribute the
  entire difference to notification overhead: the polling runs also have
  roughly 11% more draws per frame by the run-level medians, and the exact
  measured scenes/timeline are not verified. The report's independence-based
  significance argument does not remove that workload confound.
- Reported shader preparation is unusable (`stale=280`, prewarm total zero),
  and CSVs contain 210-232 runtime pipeline builds per run. Original startup
  logs and the executable/shader/manifest versions are needed to distinguish
  missing shader assets from manifest fingerprint mismatch. Do not bypass
  stale-cache validation based on this report.
- A baseline-2 sample has mean 118.4 and 122.2 draws/frame in the first two
  500-frame blocks, versus approximately 784-955 in later blocks. Its first
  pipeline build is at CSV row 1085, whereas no-suspend-notify-5 first builds
  at row 597. This is evidence of different workload progression, not proof
  that the early rows are menus. The CSV has no original present index,
  scene marker or wall timestamp to verify the claimed race-only selection
  and 600-frame warm-up exclusion. Do not arbitrarily trim another 600 rows
  and call that a corrected benchmark.
- The report's median frame duration of 11.8 ms corresponds to 84.7 FPS at
  that duration. Its approximately 95.7 run FPS is a separate statistic from
  mean frame time; those are not interchangeable. Neither CPU draw recording
  time nor this CSV alone measures GPU execution time.

Follow-up evidence worth requesting when available: the original startup and
frame logs, benchmark extraction script, executable commit/hash, custom render
thread patch, and a matching shader pack/manifest. Compare matched scene/race
time windows, paired runs and all excluded/failed attempts before changing
defaults. Analysis output is retained in
`out/thor-race-perf/friend-report-analysis.json`. No game settings or device
installation were changed while reviewing the report.
