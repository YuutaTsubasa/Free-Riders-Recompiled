# Issue 20 and performance investigation

**Baseline:** v0.4.0, commit 5e07175. Worktree `camera-debug`, branch
`codex/issue-20-performance`. Unrelated primary-checkout work is preserved.

## Completed

- [x] Reproduce the reported native-shader attachment failure with the real
  release pack; deduplicate stable stages and replace recycled guest-address
  bindings only after successful original construction.
- [x] Verify shared stages, different-shader address reuse, invalid attachments,
  and 2,100 repeated loads on D3D12 and Vulkan (red/green regression).
- [x] Trace controller posture corruption to overlapping body-record fields;
  suppress only identified live-joint writes for validated controller riders.
  Preserve animation, camera/Kinect, AI and existing player routing.
- [x] Test both local players, swapped/shared routing, connection changes and
  partially initialized/retired records; inspect runtime riding, held-item,
  stance-change and jump captures.
- [x] Measure completed-fence wait overhead; retain the D3D12/Vulkan fast path
  with correct event consumption/fence reset and the original pending fallback.
- [x] Correct the independently reproduced Windows zero-timeout WaitAll bug;
  verify atomic consumption and cancellation.
- [x] Investigate condition-variable and CPU-event polling alternatives. Remove
  both experiments because measurements did not establish a safe improvement;
  the CPU polling experiment increased early-race stalls.
- [x] Preserve 60 FPS pacing and the 1/60 simulation step. Compare HUD timing,
  record interval/pacing/queue metrics, and check scene alignment with captures.
- [x] Identify the main thread running on E-cores and worker wake-up convoys.
  Apply Windows main-thread placement and migration within the existing host
  pool, preserving guest-core synchronization and explicit CPU Sets.
- [x] Compare placement policies in the same executable: Vulkan 28.92 vs
  54.67 FPS, p95 54.08 vs 22.30 ms over 1,600 aligned race intervals.
- [x] Provide log capture and analysis tools, a low-end-device A/B procedure,
  and a report distinguishing confirmed corrections from unverified symptoms.
- [x] Independent code review; final Windows C++ 114/114, Python 248 tests
  (11 skipped), Linux targeted 4/4, real-pack shader tests on both backends.
- [x] Build a separate local package at `out/issue-20-play` in the main checkout.
  No release, remote issue comment, merge or push is part of this task.

## Limits and follow-up validation

- [x] Final-package Vulkan repeat: 57.37 FPS / 20.03 ms p95; D3D12 with XAudio2:
  54.23 FPS / 22.00 ms p95. D3D12 reached its intentional 14,500-present stop;
  no shader attachment failure. These final runs are not another matched pair.
- [ ] Reporter validation of exact character/gear posture and repeated scene
  transitions on the affected device; physical camera/2P gameplay validation.
- [ ] Repeat paired measurements on affected lower-end hardware. The measured
  Windows CPU-placement gain cannot establish gains on all CPUs or Android.
- [ ] Investigate remaining costs if affected-device measurements still show
  stalls; this patch does not fix all D3D12/low-end performance problems.
- [x] The user-approved follow-up adds true internal resolution independently
  of output size; see [follow-up validation](../../performance-resolution-validation.md).

Details and measured results: [Investigation](../../issue-20-investigation.md).
