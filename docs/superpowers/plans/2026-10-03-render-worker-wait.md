# Render worker wait experiment implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Determine whether reducing D3D12 worker empty-queue polling lowers CPU cost without hurting frame delivery on Ally X.

**Architecture:** Preserve the existing SPSC queue and all its synchronization. Compare a private executable containing a single waiting-budget change with the exact baseline. Promote the change to production source only if matched measurements support it.

**Tech Stack:** C++20, Windows D3D12, existing native presentation oracle, Python/PowerShell benchmark orchestration over SSH.

## Task 1: Baseline and candidate

Files: `out/cpu-profile-v047/ally-bench.py`, `out/cpu-profile-v047/build-candidate.py`, private copied `out/build/cpu-wait32-v047/native_presentation.cpp`.

- [x] Record CPU sampling evidence and executable hash. The performance failure is the recording worker spending most of its CPU samples in repeated yielding. This is not a unit correctness failure.
- [x] Deploy a checksum-verified baseline and independent save to `C:/Users/sinma/FreeRiders-Codex-Tests/cpu-v047`.
- [x] Start `python out/cpu-profile-v047/ally-bench.py run baseline-a d3d12`.
- [x] Build a private candidate using the baseline's exact source and objects, changing only:

```cpp
for (int i = 0; i < 32 && !found; ++i) {
```

The baseline line uses `2000`. Assert exactly one matching line. Keep `wait_for_head` unchanged. Use `out/build/render/provenance.json` to recover the native presentation compile command, and `out/build/cpu-profile-v047/link.rsp` for the link; replace only the native presentation object and output locations. Preserve command line, source diff, executable and object hashes.

## Task 2: Matched device runs

- [x] Archive completed baseline: the initial `baseline-a` was rejected because `SFR_PROFILE=0` disabled the local player profile and stayed in menus. Corrected `baseline-profile-a` uses `SFR_PROFILE=1`, reaches the race and exits at `STOP benchmark-window`. Do not include the rejected menu data in comparisons.
- [x] Transfer candidate under a separate filename, verify remote SHA-256, run candidate-b with identical settings.
- [x] Run the original baseline again as baseline-c. Keep shader/prewarm conditions and power mode consistent; report any warmup or scheduling differences.
- [x] Analyze only the middle 100 seconds of each uncapped 120-second race window. CPU deltas use the middle approximately 85 seconds. Report FPS, median/P95/P99 frame time, CPU milliseconds per frame and per elapsed second. Exclude menus and preparation.
- [x] Repeat candidate-d and bracket it with baseline-e because tail stalls and baseline FPS drift prevent a decision from the first pair.

## Task 3: Regression and disposition

- [x] Candidate-linked `native_presentation_test` passed for D3D12 and Vulkan, including asynchronous order, 6,000 records across queue capacity, destruction drain, sticky recording failure and prior upload cleanup.
- [x] Independent scope and concurrency reviews passed; subsequent measurement review recommends HOLD. Existing behavior tests plus measured performance were used rather than a test checking the constant.
- [x] Keep production waiting unchanged: reduced process CPU did not establish an FPS benefit, and frame delivery needs a stronger acceptance case. Record the candidate as HOLD; no production integration.
- [x] End only owned test processes, unregister completed test tasks, release the temporary wake request, retain normal launch settings and original saves. No release published.

## Next investigation

After this isolated experiment, inspect main-thread `special_word` call sites and actual registry size; do not combine that work into this A/B or claim the worker experiment resolves the guest's single-core limit.

## Current disposition

HOLD: two candidate runs reduce process CPU/frame by about 13–18%, but do not improve main-thread CPU or establish an FPS gain. Both have a frame exceeding 100 ms in the middle race window; the first three baseline middle windows have none. Baseline A itself has substantial stalls, so this does not prove causation. Record/draw cost also increases slightly and repeatably. Production waiting remains unchanged.

The requested scheduler captures have since completed with no lost events. They identify several smaller main-thread waits on diagnostic stderr locking, but do not explain every large stall. A later original-budget baseline also records a 576.8 ms frame, so large stalls are not exclusive to the candidate. Direct-file logging then removed an instrumentation confound for subsequent memory-path experiments; the production launcher already used file handles. The traces, untraced worker runs and later memory runs must not be pooled into one FPS claim. Evidence is retained in `out/cpu-scheduler-v047` and `out/cpu-profile-v047`; all owned tasks, games, recorder sessions and wake requests were cleaned up. Do not rerun this experiment without a new hypothesis.
