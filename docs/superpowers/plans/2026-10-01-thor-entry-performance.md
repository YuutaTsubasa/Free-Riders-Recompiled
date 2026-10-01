# AYN Thor race function-entry performance

**Goal:** Reduce repeated function-entry overhead identified by the current device CPU profile, without changing guest clocks or synchronization.

**Evidence:** A 30-second race CPU-clock profile has 18,009 samples and zero loss. The main guest uses 18.74 CPU seconds; a busy worker uses 8.575 CPU seconds. The worker spends 6.56% of its sampled leaf time in `enter_function_observed`, with a further 9.3% in dynamic TLS resolution across its helpers. Normal race frames have no pipeline compilation and considerable main ready-queue time.

**Design:** Split the rarely needed audit body out of `enter_function_observed` into a non-inlined function. Keep parallel entry handling, checkpoint cadence, naming, and watched checks in their original order. Cache a reference to the existing thread-local entry within the wrapper. No new scheduling policy, affinity, clock changes, or skipped graphics.

**Validation:** Compare emitted ARM64 hot-entry prologue and race CPU profiles. Run the existing entry/reservation and scheduler tests. Compare fixed-save race sequences with screenshots and frame summaries, accounting for route/thermal variation. Retain only improvements supported by measurements; a lower function cost alone is not a proven FPS gain.

- [x] Capture the baseline race profile with matching unstripped symbols.
- [x] Split entry audit body and build candidate.
- [x] Run entry and scheduler regression checks.
- [x] Complete comparable active-race phase measurements (routes remain nondeterministic).
- [x] Review and document results; install a normal local test APK for validation. No release or FPS speedup claimed.

## Follow-up experiment: targeted permit wakeups

The baseline scheduler broadcasts on every queue/ownership change even though only the first ready waiter can proceed. A private device benchmark of the real scheduler took 1,256 ms wall / 1,976 ms CPU for 8,000 handoffs among eight threads, and 4,454 ms wall / 6,883 ms CPU for 16,000 handoffs among sixteen threads. This isolates an avoidable wakeup convoy; it does not predict race FPS.

Test a per-queued-waiter condition variable, waking only the eligible queue head during ordinary operation. Keep the existing state condition variable for observers. Stop/failure/follower cancellation must wake every queued waiter. Preserve FIFO, urgency, companion-permit order, reservations and all guest timing. Add many-waiter FIFO and cancellation regression coverage, compare the isolated benchmark, then test the real game. Entry split alone has not demonstrated an FPS gain: race routes and draw counts differed substantially across runs.


## Verification so far

- ARM64 ordinary observed-entry stack frame: 800 bytes before, 64 bytes after; watched calls tail-call the audit body.
- Actual Windows game smoke with `SFR_DIAGNOSTIC_ENTRIES=1` reached 180 presents and logged the explicit traced entry `0x824d22f0`.
- Windows: five applicable tests passed (execution, memory, entry state, critical sections, threads); execution suite also passed five repeated runs.
- Android: native execution suite passed on AYN Thor. Independent review found no production wakeup issue; its test destruction-order finding was corrected.
- Alternating baseline/candidate/candidate/baseline device microbenchmark: eight contenders average wall 1289.05 -> 350.996 ms and CPU 1990.07 -> 360.646 ms; sixteen contenders wall 4330.605 -> 766.462 ms and CPU 6736.69 -> 773.324 ms. Two contenders were effectively similar (wall 55.390 -> 58.081 ms, CPU 62.644 -> 61.551 ms). These are isolated handoff costs, not gameplay speedups.
- Excluded the entry-repeat run from race comparison: screenshot at present 12600 still showed the pre-race overlay, unlike the baseline's active race. All raw data and exclusion reasons remain in ignored `out/thor-race-perf/`.


## Final device result (2026-10-01)

Both extended runs reached present-limit 15600. Matched approximately race seconds 10-42 using each run's screenshot HUD clock instead of absolute present number:

| Build | FPS | Average draws/frame | Main ready wait/frame | GPU wait/frame |
| --- | ---: | ---: | ---: | ---: |
| Baseline repeat | 22.73 | 830.76 | 8.60 ms | 0.86 ms |
| Entry split + targeted wakeups | 22.96 | 833.10 | 6.72 ms | 0.83 ms |

These are 720-frame windows with similar draw counts, but not deterministic replays. About 1% FPS difference is insufficient to claim a gameplay speedup. Main ready-wait time fell in this sample; the isolated handoff benchmark establishes the optimization's narrower benefit. The first, cooler baseline with fewer draws was 23.94 FPS. Remaining performance work needs to target guest CPU execution and graphics preparation, not infer a GPU bottleneck from FPS alone.

The candidate's race HUD advanced 25.08 seconds over 25.084 wall-clock seconds (presents 13200-13800), confirming normal game timing in that interval. Full 30-second profiles: candidate 19,424 samples, repeat baseline 18,934 samples, both with zero loss. Both are CPU-clock userspace profiles, not measurements of blocked/kernel time.

Final Android execution regression test passed after the test-lifetime correction. The installed normal APK is `0.4.5-perf-thor-test` (version code 15, Android 10+); no profileable/debuggable manifest flag and no `debug.env` remain. Normal launcher was opened. User saves were preserved; automated runs used a separate fixture save.

APK: `out/android-thor-performance/FreeRidersRecompiled-0.4.5-thor-test-android10-arm64.apk` in the main checkout. SHA-256: `1e198f0b0707a1fcac10a8cd3f3cfaa44727ee1c6ee88a3d3f71e9cbfd3fae13`. This is a local test build, not a published release.
