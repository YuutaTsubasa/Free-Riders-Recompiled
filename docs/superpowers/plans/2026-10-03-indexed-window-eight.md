# Indexed vertex window 8x experiment

> **For agentic workers:** Use superpowers:subagent-driven-development with sequential scope and quality review.

**Goal:** Determine whether a modest expansion of the existing GPU-indexed path reduces main-thread work without worsening frame delivery on Ally X.

**Evidence:** The local sparse-span capture found 26 additional draws per present eligible at 8x, representing 53.50% of remaining gather output. If uncached, those draws would instead add about 8.17 MiB of raw upload data per present. Wider thresholds increase that penalty. This motivates an experiment, not an accepted optimization.

**Design:** Copy the frozen benchmark graphics hook. Change only `block<=4*order.size()` to `block<=8*order.size()`. Keep its 8 MiB cap, restart exclusion, checks, indexing/base-vertex values, renderer cache, full rendering and normal game/UI time. Link the accepted memory preflight, original worker budget and original benchmark harness. No observation probes, lifecycle patch or new cache. Control is the accepted-memory 4x executable (`35b701374f0f85f353f85e767c229bdc62bb6662d0b6723a047863c5b739a403`).

## Steps

- [x] Verify idle processes and one bounded Ally connectivity check. If unavailable, continue offline preparation without repeated connection attempts.
- [x] Build a private one-line candidate with baseline object reproduction, exact 202-object/18-library provenance and map/process-start RVA. Validate scope; do not add tests that merely assert the changed constant. Existing index/bounds correctness evidence and actual runtime rendering coverage remain separate from this performance hypothesis.
- [x] Sequential independent scope and quality review. Derive fresh direct-file benchmark cases from the reviewed harness; verify only executable identity, case paths and API differ as intended.
- [x] If Ally is available, run fresh Vulkan control/candidate/control with independent saves/caches, known hashes, per-case bounds and temporary wake requests. Stop only owned processes/tasks; restore normal launch/sleep behavior.
- [x] Compare main/process CPU per frame, FPS, frame-time tails, clock correctness, cold preparation and gather/upload/cache bytes. Compare logical draw/index counts; raw vertex counts intentionally increase with wider indexed spans. Do not pool earlier traced or pre-memory results into this comparison.
- [x] Preserve evidence and disposition. A reduction in gather bytes alone is insufficient. Keep private unless repeatable benefit and correctness/regression evidence justify promotion; Android remains separate validation. No merge, push, release or external messages.

Artifacts: `out/cpu-index8-v047`, `out/build/cpu-index8-v047`, plus uniquely named direct-file cases. Never overwrite completed experiments.


## Result and disposition (2026-10-03)

**Promising, private only.** Fresh Ally X Vulkan control/candidate/control completed with the accepted-memory executable as both controls. No production threshold change, cache implementation, private lifecycle patch, Android install, merge or release.

| Metric | Control V (4x) | Candidate W (8x) | Control X (4x) |
| --- | ---: | ---: | ---: |
| Main CPU ms/frame | 9.920515 | 9.269848 | 9.910336 |
| Process CPU ms/frame | 19.377619 | 18.568770 | 19.487360 |
| FPS | 72.43753 | 75.63013 | 72.00972 |
| Frame p99 ms | 16.7810 | 16.1296 | 16.6721 |
| Worst frame ms | 26.8035 | 37.2208 | 70.0901 |

Main CPU cost is 6.56% / 6.46% lower than the separate controls; FPS is 4.41% / 5.03% higher. Control main CPU drift is about 0.10%. This is one candidate run in a stationary Dolphin Resort scene, not statistical significance or a general device/cross-course result. The candidate and last control each have one frame above 33.33 ms; no uniform tail-latency improvement is established.

The work counters support the hypothesized mechanism: gather time falls from 0.696/0.661 to 0.340 ms, swap time from 0.102/0.096 to 0.050 ms and repack time from 0.294/0.293 to 0.146 ms. Per-frame supplied vertex span grows about 8.3 MB, while cached span grows about 10.25 MB and uncached swap bytes fall about 1.96 MB. Those are overlapping/path-dependent counters, not total PCIe traffic or unique residency. Mean completed draws differ by at most about 0.31%, and original guest index counts by about 0.08%; workloads are comparable, not frame-identical. No upload-budget drains or ring flushes occurred in these measurement windows.

All three app caches were independent, but driver caches were not reset. Startup prewarming of 336 recipes took 98.7 / 46883.4 / 91.9 ms. The candidate executable was being launched for the first time while controls had been used before. This is a startup difference with uncontrolled driver-cache warmth, not evidence that the threshold itself adds 47 seconds. Startup and steady-state conclusions must remain separate.

Primary visual checks at frames 14400 and 19200 found the complete stationary scene and normal clocks: HUD deltas 66.02 / 63.61 / 66.77 s versus logged 66.0052 / 63.6030 / 66.7804 s. This is not pixel-equivalence, lap-completion, audio-quality or full-course validation.

Scope and quality reviews passed for the exact one-byte candidate, case harness and analyzer. The analyzer rejects gaps crossing the measurement boundaries. A further correctness audit found a generic supported-layout boundary: 16 DEC3N elements at stride 16 can expand an 8 MiB raw span to 72 MiB host data, exceeding the 64 MiB uncached upload ring; the prior gathered representation for that newly eligible ratio-8 draw is 9 MiB. This is not an observed game failure, but a reason to validate host-expanded bounds before adopting a wider threshold. Existing cache lifecycle limitations also remain; the private lifecycle experiment was not linked into these binaries.

Candidate SHA256: `763b9d0bee2be5deccece36d101263a83bf7208a0583c538f96d71c2c09a8d32`.
Control SHA256: `35b701374f0f85f353f85e767c229bdc62bb6662d0b6723a047863c5b739a403`.
Both process-start RVAs: `0x748b5e0`.

Evidence: `out/cpu-index8-v047/analysis-results/results.json`, `visual-clock-verification.json`, `device-cleanup.json`, and the raw V/W/X directories under `out/cpu-profile-v047`. Batch and all games completed; scheduled tasks were removed, source fixture hashes match, Turbo power scheme stayed unchanged and wake session 9 was released. Ordinary launcher settings were never modified.

Next: retain this positive A/B/A result without rerunning it automatically. Before promotion, establish an inexpensive correct host-expanded upload bound, retain/review index and cache invalidation contracts, and then repeat a matched candidate/control validation plus broader scenes and APIs. A revised candidate needs its own source hash and results. Android remains separate. Do not describe the earlier 2000-to-32 worker experiment as accepted; only the ordinary-page memory preflight remains in production source.
