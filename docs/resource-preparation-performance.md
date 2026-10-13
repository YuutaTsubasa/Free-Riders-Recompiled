# Performance changes and evidence

This change reduces repeated draw preparation while preserving guest updates, queued resource versions and GPU retirement. It also adds a strict gameplay benchmark protocol so loading and preparation cannot be mistaken for racing. The target results do **not** establish stable 60 Hz or a fix for charge/start-input lag.

## Implementation

- **Constants:** retain VS/PS shadows and dirty intervals, convert the union of changed ranges once, and reuse immutable upload-ring slots by owner/version. Draw records borrow constant views instead of embedding two 4 KiB arrays. Bool/loop constants use the same versioned lifetime; an independent original-setter/GPU fixture also corrects the previous loop-source offset. Ring rotation and capacity flush invalidate affected uploads before reuse.
- **Draw and shader state:** borrow decoded vertex-layout views, retain the selected pipeline, and avoid redundant root/vertex/index bindings within a command-list generation. A bounded shader-pair memo avoids repeated object-map lookups while preserving attach-generation invalidation and consume-time checks. Fresh command lists restore conditional index/palette bindings.
- **Texture/sampler and stencil state:** publish versioned fetch snapshots and pending stencil words at audited update/reset/embedded-record boundaries. Clean draws reuse decoded state. Descriptor reuse also requires the current texture generation; texture-content watches, alias checks and GPU fences remain. Stencil reference is published into the pipeline descriptor so existing command-list memoization can avoid redundant native calls.
- **Buffers and resource identity:** keep index-scan metadata with its existing GPU cache entry, reuse recent managed-buffer selections, and reject cached data when the caller selects a different guest alias. Immutable queued snapshots and deferred retirement survive update/free/remap/reuse. Extra Unlock-managed buffers and prepared formats remain optional; the existing index cache is retained, not counted as a newly invented optimization.
- **Diagnostics:** per-draw timers, texture timing and first-seen draw/texture histories are explicit diagnostics. Disabled timing is reported as unavailable rather than zero work. No rendering, effects, audio or simulation ticks are omitted to obtain the reported results.

Direct full-stage constant upload remains a separate experiment. Unconfirmed writer paths and full alias/page validation retain conservative live-read/fallback behavior. The PR port excludes the separate ultrawide, launcher and native-shortcut work.

## Local correctness evidence

CPU fixtures cover update masks and intervals, original setter expectations, missed-writer detection, source aliases, partial/dynamic updates and resource reuse. D3D12 and Vulkan GPU readbacks cover queued old/new resource versions, constant/format results, stencil faces/reference, fresh-list index binding and prewarmed pipelines. Palette-slot restoration has source review rather than a dedicated GPU readback case. Bounded game segments exercised the independent constant, texture and stencil verifiers without recorded mismatch/GPU-fault stops; they do not establish all-scene coverage.

Verification modes compare exercised source data independently and fail on unnotified changes. They are excluded from performance measurements. These tests establish correctness and avoided operations for the exercised paths, not a target-machine speedup.

## Target performance

Hardware: Core i5-3470, Radeon RX 480, DDR3-1333. The latest comparison used uncapped D3D12, 1280×720, original aspect, 100% rendering, audio enabled and fixed-step Time Attack. Both versions enabled managed constants/texture/stencil, with extra managed buffers and direct constant upload disabled.

The strict suite validated all 17 required runs: one verification run, two same-binary control pairs and six comparison pairs. Each run includes **300 countdown + 5,100 racing ticks** with checked configuration, hashes, settings, route and expected stop/exit. Loading is excluded using observed gameplay progress, while the complete countdown and final countdown transition are retained.

This compares **`4e72384` → `201b50f` only**, with packaging/protocol checkpoint `263604a`. Both runtime versions already contain the earlier resource/state work. The baseline always timed texture lookup; the candidate disabled that diagnostic. This comparison does not measure the total benefit of the broader changes.

Each FPS entry below is the median of six per-run FPS values. P99 columns are the median of the six per-run nearest-rank P99 values, plus the candidate's worst replicate P99; they are not pooled percentiles.

| Segment | Before FPS | After FPS | Before P99 ms | After P99 ms | After worst P99 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Countdown | 45.786 | 45.400 | 63.539 | 67.158 | 71.116 |
| Racing | 51.801 | 51.715 | 50.798 | 51.391 | 54.218 |
| Countdown + racing | 51.497 | 51.388 | 51.231 | 52.770 | 55.756 |

The median paired racing FPS ratio is **0.9980**. The two control ratios span 0.9932–1.0058. This provides no demonstrated improvement for the subsequent batch. Candidate worst-replicate P95 is 27.213 ms for countdown and 24.004 ms for racing; all countdown/racing/combined targets miss P95 ≤15 ms and P99 ≤16.67 ms. The largest candidate countdown interval is 275.337 ms, and the largest combined interval is 318.136 ms.

Verification and controls were reused after an interruption before comparison. Original context and all rounds are retained; the earlier control spread and pre-run CPU snapshots do not establish noise or continuous quiet during the later comparison. Present intervals do not separate CPU and GPU cost.

An earlier legacy window reported **38.38 → 60.13 FPS**, with median paired ratio **1.5755** (approximately +57.55%). Subsequent frozen-log inspection showed different loading/preparation workloads at equal `racing=1` record indices. That result is confounded and must not be presented as a verified gameplay gain. Extra managed buffers reported 56.31 FPS versus 60.13 FPS, paired ratio 0.9428 and slower results in all six legacy rounds; they remain opt-in, with the precise regression magnitude also subject to that window limitation.

Earlier experiments did not establish a benefit from content-comparison constant reuse or reimplementing the already-effective index cache. They did expose diagnostic timing overhead, motivating separate low-overhead measurements.

## Remaining evidence

Displayed 60 Hz, input-to-display latency, charge/start response, heavy Free Race, multi-track/full-race reentry and long-session acceptance remain open. No reliable CPU-function ranking is available: the recovered diagnostic trace retained late recorder activity rather than the gameplay samples, and the offline coverage gate rejects it. Do not infer a bottleneck or optimization gain from that failed trace.

The [sanitized per-run evidence](performance/i5-resource-preparation-2026-10-13.json) retains all17 runs, segment statistics, executable hashes, paired ratios and acceptance flags. Its short build labels identify development snapshots, not commits in this PR's upstream-based history. It contains no local paths or operational session metadata.

## Controls and defaults

The update-publication paths are opt-in: `SFR_MANAGED_CONSTANTS=1`, `SFR_MANAGED_TEXTURE_STATE=1` and `SFR_MANAGED_STENCIL_STATE=1`. Extra buffer interception (`SFR_MANAGED_BUFFERS=1`) and full-stage direct uploads (`SFR_DIRECT_CONSTANT_UPLOAD=1`, requiring managed constants) remain separate experiments; do not enable them as a proven speedup. Existing fallback rendering remains available.

For correctness, each enabled state path has a matching `SFR_VERIFY_MANAGED_CONSTANTS`, `SFR_VERIFY_MANAGED_TEXTURE_STATE`, `SFR_VERIFY_MANAGED_STENCIL_STATE` or `SFR_VERIFY_MANAGED_BUFFERS` flag. Verification detects changes missed by publication hooks and stops rather than silently reusing stale state. `SFR_BUFFER_AUDIT=1` adds lifecycle diagnostics. Verification/auditing costs are excluded from performance comparisons.

Use `SFR_FRAME_METRICS=1 SFR_DRAW_TIMERS=0` for frame-level measurements. Draw signatures, texture timers and texture first-use history require `SFR_DRAW_SIGNATURE_TRACE=1`, `SFR_TEXTURE_TIMERS=1` and `SFR_TEXTURE_TRACE=1` respectively. The existing graphics-trace diagnostic can still request detailed draw timers.

The semantic benchmark is restricted to the audited `solo_fixed_v1` route. `scripts/benchmark.ps1 -GameplayTicks 5400` validates complete300 countdown +5100 racing ticks, unchanged route/configuration, normal audio/full rendering and expected termination; its semantic modes and companion report validator reject legacy or incomplete observations. It does not prove other tracks or Free Race. `benchmark_codex_i5.ps1` handles prepared target kits, six interleaved pairs, two control pairs, isolated local fixtures and explicit verified-stage resume. It is not a remote-deployment tool.

## Resource contracts and review scope

This follows event publication and immutable version/lifetime principles examined in [UnleashedRecomp video.cpp at cf829a9](https://github.com/hedge-dev/UnleashedRecomp/blob/cf829a9eca8fb680fba4b0409ddeb6ca92f22e3c/UnleashedRecomp/gpu/video.cpp), adapted to this title's audited writer addresses. It is not a wholesale port of that renderer or a promise of equivalent performance.

- Constant state is invalidated after the original setter preserves guest behavior. VS and PS ownership/version identity remain separate; ring wrap/command-list restart cannot reuse an overwritten allocation.
- Shader attachment generation invalidates resolved object pairs; queued native shader ownership survives later address reuse.
- Texture fetch-state publication is distinct from content-generation validation. Source write watches, aliases, reset and GPU retirement remain authoritative.
- Managed buffers publish completed write ranges into immutable snapshots. The selected guest alias, source/version, format and range are checked; GPU cache metadata retires with its actual resource. Unknown paths keep guarded fallbacks.
- Stencil reference and conditional descriptor/index bindings are republished for a fresh command-list generation.

The PR is based on upstream `a5ac758`. Author camera, gesture, title-statistics and saved-window behavior are preserved. Private handoff/automation/deployment files, profiling-session tools, game data and unrelated aspect/shortcut changes are excluded.

## Validation of the upstream integration

See the PR description for exact local checks on this port. Previous complete development race segments are supporting evidence only; they do not validate new upstream gameplay or its target performance. A normal full-rendering, audio-enabled i5 comparison against upstream, including countdown, heavy Free Race and actual displayed60Hz, remains required before claiming a performance gain or completion.

## Required comparison against the official v0.7.3 release

The requested primary control is the author's **unmodified official Windows v0.7.3 release**, from [the published release](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.7.3). Its source tag resolves to `72ad76e8584d58eddb3a457afcabe115b86c066c`. The PR base `a5ac758` differs from that tag only in README files and `.recomp.json`; the runtime source is unchanged. This source relationship does not establish binary identity: record the exact official download asset and executable SHA-256 before testing.

The existing17-run report compares two earlier development snapshots, not v0.7.3. **No performance benefit against the official v0.7.3 executable has been measured yet.** Correctness tests and reduced operation counts explain the mechanism; they do not replace that comparison. Implementation and measurement are currently paused; the following is a proposed acceptance protocol, not a completed test.

1. **Freeze both binaries.** A is the original official v0.7.3 executable. B is a fully built, fixed PR revision. Record both executable hashes, revision/release provenance, compiler/build information where available and every setting. The existing development play package is not a substitute for B. To attribute a product-level difference specifically to source changes, separately compare source builds of v0.7.3 and the PR using identical compiler/options; do not silently substitute such a build for the official primary control.
2. **Use the same target and workload.** Use the i5-3470/RX480, D3D12,1280×720,100% rendering, normal audio and complete effects. Fix character, gear, track, opponents, route, simulation/timing controls, game files and copied save fixtures. Begin each run with equivalent warm shader/pipeline state; report cold first-entry behavior separately. Disable intrusive per-draw, verifier and CPU-stack diagnostics on both. Keep extra managed buffers/direct uploads off for the main comparison; evaluate them separately if requested.
3. **Estimate noise, then interleave.** Include at least two identical-v0.7.3 control pairs and six A/B pairs, alternating the order between rounds. Preserve every round and failures; a repeat is valid only for an explicitly documented invalid run. Require idle/quiet admission, no other game or heavy work, and record power/clock/environment context. A control or pre-run snapshot does not prove uninterrupted quiet throughout later runs.
4. **Measure displayed frames externally.** Use the same version/settings of [PresentMon](https://github.com/GameTechDev/PresentMon) for per-frame application/display data on both executables. Keep a common60Hz cap/display test separate from an uncapped throughput test; do not compare a capped A with an uncapped B. Report unavailable metrics as unavailable, not zero. Keep CPU sampling out of the timed comparison.
5. **Retain the whole countdown and align phases.** Separate loading, the complete3D countdown/start transition, racing and combined countdown+racing. Release-binary frame captures do not contain the new game-tick observer: phase labels require synchronized on-screen evidence, a disclosed boundary precision and rejection of ambiguous/missing-countdown runs. Do not use equal `racing=1` indices or remove the first slow countdown frames. If exact300+5100-tick analysis is required, make separate measurement-only observer builds of both source versions, check instrumentation overhead, and label those results as instrumented builds rather than the untouched official release.
6. **Judge consistent benefit and tails.** Publish each paired FPS ratio, its median and uncertainty alongside identical-binary control variation. Claim a measurable benefit only when it is consistent and exceeds the observed measurement variability; otherwise report inconclusive/no demonstrated gain. Also report per-run P95/P99/max frame time, frames over33.3ms and available display/missed-refresh data for every phase. A higher mean FPS with unchanged/worse long frames is not stable60Hz. The target remains P95≤15ms/P99≤16.67ms for uncapped work, plus actual60Hz display acceptance; throughput and displayed cadence are separate results.
7. **Check normal play.** Verify full-race results/reentry, heavy Free Race with opponents/effects, at least three representative tracks and a30-minute normal60Hz session. Include charge and precise-start response with a separate input-to-visible-response measurement. A CPU leaf ranking or automated solo route cannot establish those conditions.

Attach sanitized per-run data, settings/hashes, phase alignment evidence and a summary that distinguishes demonstrated throughput changes, tail/display changes and unverified acceptance items. If this protocol shows no improvement, retain the result and revise the optimization plan; do not relabel the earlier legacy window as proof.
