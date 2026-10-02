# CPU bottleneck follow-up after v0.4.7

Status: user approved this design on 2026-10-03. The worker-wait experiment is complete and remains unaccepted; its production budget is unchanged. Subsequent common-path work accepted only the ordinary-page memory preflight, recorded in `../plans/2026-10-03-memory-preflight-experiment.md`. Read the latest continuation before starting more tests; the completed stationary comparisons must not be restarted automatically.

## Scope and evidence

Prioritize Windows and ROG Xbox Ally X, then Android. Preserve normal game and UI time, audio, every required draw, input routing, and independent test saves. No release is part of this experiment.

The local D3D12 stationary Dolphin Resort capture uses v0.4.7 runtime sources with only a private bounded timing harness. ETW recorded 35 seconds of race sampling, with zero lost events or buffers. The captured executable SHA-256 is `aeb78c36ec6e6c7d79e6b048283fdebd0a295665801b98fabb0be245cb4b2426`. Resolve addresses with its exact linker map and ETW image base `0x00007ff60d040000`.

There are 68,680 process CPU samples, including 23,940 on the main thread. `native_draw` appears in 20.60% of main-thread stacks; `GuestMemory::special_word` accounts for 2.31% of main-thread leaf samples. Inclusive measurements overlap and must not be added. Some kernel samples include interrupts. Background applications and diagnostic logging prevent treating this run as a controlled FPS comparison.

The D3D12 recording worker consumes approximately 31% of process samples. Most worker samples have the `run_render_thread` -> `SwitchToThread` path. The source repeatedly calls `std::this_thread::yield()` up to 2,000 times after the queue empties. This is a candidate for reducing wasted host CPU; it is not proof of main-thread speedup. Android and default Vulkan do not use this worker.

## Alternatives

1. First bound the worker's empty-queue yielding much more tightly, then sleep using the existing condition-variable handshake. This leaves recording order, resource ownership, error propagation and rendering unchanged. It may reduce CPU consumption, but excessive sleeping could increase producer stalls.
2. Publish command batches to the worker. This could amortize wakeups but requires changes to publication/drain boundaries and more substantial synchronization validation. Defer until simpler waiting changes are measured.
3. Optimize the common main-thread memory and rendering paths first. This benefits both backends, but each path requires separate contracts and evidence; the already-rejected matrix fast path must not be silently reinstated.

## Proposed first experiment

Compare the existing worker with a private candidate that lowers only the worker's empty-queue yield budget from 2,000 to 32. Leave the producer's drain wait unchanged to isolate the variable. Keep the current mutex, sleeping flag, stop/error state, sequence ordering and notifications. Do not batch, drop, reorder or defer required commands.

Use matched baseline/candidate/baseline runs on Ally X, same executable provenance except this change, same scene, API, resolution, save fixture and device power mode. Collect whole-process and per-thread CPU time, race FPS, frame-time median/P95/P99, stalls, and evidence of normal game time. Use bounded tests and temporarily prevent sleep without changing the power plan. Restore ordinary launch behavior after testing.

Accept only if CPU time per frame clearly decreases without a reproducible FPS or frame-time regression. If it regresses or remains within noise, do not ship it. Check worker wake/drain, queue-full, exception and shutdown behavior plus the existing rendering oracle before considering integration. Keep GPU/resources and draw ordering exactly as before. Vulkan provides a control; Android validation follows only for changes to shared code.

After this experiment, investigate `special_word` callers and registry size before designing its optimization. No assumption that binary search or a cached pointer is safe or faster.

## Limitations

Stationary race sampling does not validate all courses, Grand Prix, two-player play or camera/VRM. A worker CPU reduction alone must not be described as resolving the single-core guest bottleneck. No generalized device performance claim without device measurements.
