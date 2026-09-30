# Handheld wait attribution plan

## Expanded authorization and experiments (2026-09-30)

The user subsequently requested autonomous performance optimization while away,
including Intro video slowdown with normal audio. The original attribution-only
scope below is retained as history. Prepare private test builds, preserve v0.4.2,
and do not publish a release. Keep the 60 FPS game limit and guest time unchanged.

- [x] Main-only wait attribution and bounded tests; separate optional worker signal graph.
- [x] Inspect Ally capture and run desktop pilot; no handheld speedup claim.
- [x] Correct demonstrated padding-dependent pipeline identities; regressions red then green.
- [x] Implement notification-based guest suspension with cancellation, nested and early-resume tests; retain polling A/B switch.
- [x] Add opt-in scoped Windows timer request; local calibration shows no established gain, so do not enable by default.
- [x] Same-binary polling/notification race runs; inspect visuals, wait durations and frame distributions. Four isolated runs after discarding overlap-contaminated comparisons; wake latency improves, overall FPS gain is not established.
- [x] Movie telemetry and playback path exercised; decoded progress remains distinct from present rate and handheld audiovisual synchronization remains unverified.
- [x] Windows 118/118, Linux 112/112, Python 266 completed (11 skipped); Android build and signature/alignment checks; independent synchronization/diagnostic review.
- [x] Private candidates, capture launchers and evidence report prepared. Android hardware is disconnected; no release publication.

Do not shorten the existing pure-virtual retry or change guest job timeout behavior
without evidence of the producer's lifetime requirements. Its counted 100 us sleeps
can overshoot the nominal budget, but blindly shortening them risks skipped jobs.

## Original diagnostic design

**Goal:** Explain the Ally X capture's 18.9 ms/frame main-thread blocking without changing guest scheduling, sleep durations, graphics, or the 60 FPS cap.

**Approved scope:** The user approved opt-in wait attribution, local validation, and an Ally X test package. Reuse the free camera-debug checkout on `codex/handheld-wait-trace`, based on v0.4.2; preserve the unrelated untracked PR21 plan and the primary checkout.

**Design:** An opt-in `SFR_WAIT_TRACE=1` collector measures each main-thread native callback separately from execution-permit release/reacquisition. Group by wait kind, guest caller, targets, requested timeout and returned status. Report bounded aggregates every five seconds at Present and flush the final partial window at exit. Existing `SFR_WAIT_GRAPH` signal counts can supply hints about event producers; they are not causal wakeup evidence. Record all targets for multiple waits, never attribute WaitAll to its first object. Include critical sections, single/multiple object waits, explicit delays, self-suspension, frame pacing and GPU wrapper waits. Disabled mode takes the original callback path without extra clock reads, allocation or logging. No timer-resolution or scheduler fix in this change.

**Limitations:** Native callback duration includes host scheduling delay and is not pure CPU/GPU time. Post-callback duration includes permit reacquisition and bookkeeping; it does not prove the instant an event was signalled. Finite timeout overshoot is meaningful only for a timeout result or explicit delay, not a successfully signalled wait. Signal counts omit some asynchronous producers and do not identify which signal satisfied a wait. Instrumentation overhead must be checked locally, not assumed zero.

## Steps

- [ ] Add deterministic failing tests for timing separation, disabled-mode transparency, exception propagation, timeout-vs-event interpretation, aggregation bounds, multi-target identity and consuming flushes (`tests/wait_trace_test.cpp`).
- [ ] Implement a small collector and callback wrapper (`src/wait_trace.h/.cpp`), then pass its tests. Add the CMake target and link it into the runtime.
- [ ] Label existing wait call sites in `diagnostic_main.cpp` without changing their run_wait/run_blocking choice. Flush through the Present hook and shutdown. Preserve existing waits and errors.
- [ ] Run wait/critical-section/sync regression tests, build the runtime, and inspect a real 1P race with trace enabled, including matching per-frame blocking and caller/target aggregates. Run a comparable trace-disabled capture to check observability overhead; no speedup claim.
- [ ] Prepare an isolated full Windows test ZIP and one-click capture script, preserving the published v0.4.2 and user settings. Include version/hash metadata, expected-output validation and clear return-ZIP instructions. Keep game assets/saves/personal models out of the distributable.
- [ ] Document local findings, review the changes, and deliver the test package. Ally X remains the required validation device; do not publish a new release or claim a fix.
