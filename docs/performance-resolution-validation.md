# Performance and resolution follow-up

This local build extends the [Issue #20 investigation](issue-20-investigation.md)
with CPU sampling, quieter normal-play diagnostics, guided feature benchmarks,
and true internal rendering resolution. It is not a published release.

## CPU investigation and normal-play overhead

The previous local executable (`15a54060...fc8ca`) was sampled with its matching
link map on the i9-14900KF / RTX 4090 test machine. The run retained the 60 FPS
cap and gathered 18,469 main-thread instruction-pointer samples after present
13,000. It includes late racing and the mission transition, so it is not a
controlled before/after performance comparison.

47.43% of samples were outside resolved executable symbols and may include
waiting. They must not be reported as active CPU work. The largest resolved
groups were native draw submission (2.92%), the indexed-draw hook (1.64%),
recompiled function `82534038` (1.47%), vector-memory stores (1.31%) and indirect
calls (1.17%). Work was spread across submission, memory handling and game code;
the sample did not justify another speculative scheduler change.

Normal launcher play now skips detailed per-draw diagnostic clocks, per-frame
stream-set construction and long frame-log formatting. Input logs retain initial
and connection-status changes without printing every advancing packet. DEC3N
scratch storage is reused per thread. Rendering, callbacks, counter resets,
write epochs and pacing remain active. Independent review found no blocking
correctness issue in these changes. Their individual FPS benefit has not been
quantified; the earlier placement comparison is not evidence for this change.

`SFR_FRAME_METRICS=1` explicitly enables consecutive detailed frame records;
both capture tools request it. Quiet-mode heartbeats are not frame-time samples.
`SFR_TRACE_INPUT=1` restores input packet logging when diagnosing controls.

## Repeatable feature captures

[benchmark_scenarios.py](../scripts/benchmark_scenarios.py) stages separate
captures for 1P, 2P, camera motion and VRM, keeping the source package/settings/save
intact. It records hashes, a user-supplied play protocol, settings and cache seeds,
and rejects crashes, incomplete logs and changed settings. Captured runs remain
unverified until the actual scene and active feature have been inspected.

The runner's 18 fixture tests pass. These validate isolation and capture handling;
they are not measurements of live camera, two-player gameplay or a real VRM.
Follow [the scenario guide](benchmark-scenarios.md) on each device.

## Internal rendering resolution

The launcher's Display settings offer 360p, 540p, native 720p, 1080p and 1440p,
independently of output window size. Default is native 100%. Color/depth targets,
resolved copies, viewport/scissor/clear commands and VRM rendering scale together.
Guest coordinates, resolved-texture guest-pixel offsets and simulation timing
remain unchanged. See [the resolution guide](internal-resolution.md).

Shader ABI 9 is required. All 476 original shader source identities were retained
and translated for both D3D12 and Vulkan. The updated runtime also bypasses stale
unversioned prepared bytecode when runtime translation is enabled.

GPU readback regressions cover all five scales on both backends: resolved-texture
sampling and offsets, ordinary textures, screen-space half-pixel geometry,
clipping, partial loading regions, split viewports, VRM depth and HUD ordering.
These are targeted rendering tests; they do not replace human two-player or
camera gameplay validation.

## Final verification

The final Windows build passes 116 CTest cases and the 266-test Python suite
(11 environment-dependent skips). PowerShell capture-script parsing and
`git diff --check` pass. The launcher settings are covered by tests; its Windows
UI has not been independently screenshot-validated in this round.

Actual final-build runs use the same scripted standard-rider course with 1P,
camera/VRM disabled, a 1280 x 720 output window and a 60 FPS cap:

| Run | Internal image | Evidence |
| --- | --- | --- |
| `resolution-final-vulkan100` | 1280 x 720 | Loading and racing screenshots inspected; 11,500 presents, intentional limit. |
| `resolution-final-vulkan50-quiet` | 640 x 360 | Menu, loading and racing inspected; quiet diagnostics; 11,500 presents, intentional limit. |
| `resolution-final-d3d12-200` | 2560 x 1440 | Menu, loading and racing inspected; XAudio2 enabled; 11,500 presents, intentional limit. |

The native Vulkan cold-cache run has a complete 1,600-interval racing window
(frames 9313..10913): 54.36 FPS, 18.40 ms mean, 22.18 ms p95 and 27.65 ms p99.
634 intervals include pipeline creation. This is a runtime smoke measurement,
not a warmed or matched performance comparison with the earlier local build.
The quiet 360p run deliberately lacks detailed frame rows; no FPS improvement
is claimed from it. Screenshots verify actual render-target sizes and game/HUD
layout rather than relying on a changed window size.

The D3D12 1440p cold-cache run has a complete 1,600-interval window
(frames 9309..10909): 57.47 FPS, 17.40 ms mean, 20.14 ms p95 and 23.24 ms p99.
590 intervals include pipeline creation. It uses a different backend, resolution
and audio setting from the Vulkan row and is not evidence that higher resolution
is faster. All three runs reached their intended present limit without a shader
stop or crash. Quiet mode produced 39 health records and no detailed frame rows.

After gameplay, the final D3D12 presentation/resolution/ABI regressions passed
again (3/3), and the delivered pack passed the 2,100-reload lifetime test on both
backends. Test games were closed before updating the separate local package;
the update verifies existing settings and save-file hashes remain unchanged.

Runtime SHA-256: `86839981cb407f0a80359127e04e83a4907a3c7c5913d5497edb29a36d739200`.
Launcher SHA-256: `6ea7c8e819e78026f2becf1123e476753ac990ac3ea660f8f8a7b7cfa629a9ab`.
Shader pack SHA-256: `10b17a4d7247914fa0af8a167948d6c5709a5da77d756f1f5d34c92c949fab71`.

The user's subsequent launcher change makes Vulkan the default for new settings
and replaces the experimental toggle with **Display > Graphics backend**,
offering **Vulkan** and **D3D12** on Windows. Existing `vulkan=0/1` and `false/true`
settings keep their chosen backend. A missing or malformed value uses Vulkan.
Both choices produce an explicit game environment value. Settings regressions
failed before the change and passed after the launcher rebuild. This changes
only the launcher; the runtime and shader-pack hashes used above are unchanged.

No affected low-end device was available. Lower internal resolution can help a
GPU bottleneck but does not remove CPU submission or game-logic costs. The game
still runs with a 60 FPS cap and 1/60 simulation step; falling below 60 FPS still
slows simulation. Do not uncap it to claim higher game performance.
