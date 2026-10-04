# Integrate memory lifecycle cache invalidation

> **For agentic workers:** Use superpowers:subagent-driven-development, with implementation, scope review, then quality review. Existing autonomous CPU/correctness work is authorized; no merge/release/push.

## Design and rationale

The completed real NativeRenderer audit reproduced stale reuse after decommit/recommit, release/map, and ordinary PhysicalMemory free/invalidate/reallocate. Both Windows backends return old filled buffers after guest backing becomes zero. Retained GPU/0x404 reuse is already correct. The separate private epoch experiment eliminated these three observations without changing renderer behavior.

Choose lifecycle write notification in GuestMemory: after all logical/native preflight checks, mark each backing range immediately before its destructive host operation. This preserves conservative invalidation if a host operation partially fails, while rejected logical requests remain clean. Use existing mark_written, so disabled watches/epochs retain their existing inexpensive behavior. Update its public write-tracking documentation to include backing destruction, without promising concurrency snapshots.

Alternatives: invalidating only in renderer.invalidate would miss raw decommit/recommit and other memory callers; replacing the epoch scheme with generations would broaden scope and overhead unnecessarily. Do not change cache keys, vertex upload thresholds, draw order, timing, worker waits, layout or allocator policy. The guarded performance binary remains frozen and separate.

## Files and implementation

- tests/guest_memory_test.cpp: add focused public-API regressions for dirty bits and epochs after partial decommit/recommit and exact release/remap, including rounded final backing page and untouched neighboring page. Include epoch-disabled dirty tracking, same-epoch lifecycle reset, stable recommit, rejected invalid/pinned operations staying clean. Reuse existing assertion style and main registration. No copied renderer algorithm and no new build target.
- src/guest_memory.cpp: minimal four notification sites corresponding to reviewed private experiment, before Windows decommit/release and POSIX replacement operations, after preflight. Explain partial-native-failure invalidation in a concise comment. Preserve validation and all ordinary access behavior.
- src/guest_memory.h: document lifecycle invalidation on watched backing ranges and conservative invalidation on possible partial native failures.
- New ignored out/cpu-lifecycle-integration-v047 and out/build/cpu-lifecycle-integration-v047 only: bounded compile/test helpers, original/red/green source and object hashes, command records, exact runtime provenance. Do not overwrite old artifacts or rebuild shared/main-checkout outputs.

## Checks and checkpoints

- [x] Add regression coverage first; compile tracked tests against pinned unchanged accepted memory and record the intended failing assertion before production changes.
- [x] Implement the minimal notifications and documentation; compile new memory and tests to isolated outputs. Run the full guest-memory suite plus existing physical/virtual memory suites and prior 35-case lifecycle oracle linked against newly compiled tracked memory. Preserve all logs and hashes; tests at most120s, compile/link at most180s.
- [x] Compare existing behavior snapshot allowing only previously reviewed lifecycle dirty events; retain explicit expected delta, not blanket snapshot replacement. Check production diff and frozen guarded binary unchanged.
- [x] Scope reviewer validates requirements and preflight/partial failure semantics; then quality reviewer validates portability, tests and no extra hot-path changes. Fix and re-review before integrating a game binary.
- [x] Fresh actual renderer probes linked to newly compiled tracked memory on Vulkan and D3D12, reviewed bounded helper and separate outputs; no game assets or presents. Require zero stale decisions and unchanged controls. Do not rerun old accepted/private audit binaries.
- [x] Save verified results and limitations in plan/continuation, local commit only. A normal-game scene-transition smoke and later Android validation remain explicit follow-up work, not inferred from standalone probes.

No gameplay reachability, pixel equivalence, performance improvement, concurrent snapshot or complete GPU lifetime proof follows from these tests. Windows runtime tested; POSIX must be identified as inspected-only unless independently run. All tests local, no remote devices, no user saves/settings, no wake request needed.

## Verified result (2026-10-03)

Implemented four lifecycle write notifications, API contract comments and four focused guest-memory regressions. The unchanged memory object failed with exactly three intended dirty-tracking assertions; the same test source passes against the tracked repair. An initial fixture attempt was preserved separately: I/O pin admission itself records writes, so the final rejection test clears that admission event and advances its reference epoch before testing rejected operations.

Full guest-memory, physical-memory and virtual-memory suites passed. The newly linked 35-case lifecycle oracle reports zero failures and zero stale decisions. The 1,209-row behavior snapshot differs only in three explicitly allowed lifecycle dirty events and their cumulative counter totals; other state, bytes and errors match. All 17 GREEN compile/link/test commands returned zero. Windows was executed; POSIX and induced native partial failures were not tested.

Independent scope and quality reviews passed before executing the newly linked actual renderer probe. Vulkan and D3D12 each completed 119 assertions, 59 observations, zero stale decisions and zero fixture failures in less than one second. Their observation rows match; each raw output also matches its prior private-lifecycle control. The fixture never draws/presents or reads upload memory. Source a4dc194cc853da84c778057a30258fc7195952824cb0fee7857ca2048806ee21; tracked memory object770fbdc7a191f373e63be29666383b097cc29e03bcb33c6c67f5a71ef9f080ce; renderer probe d1eb610b7f02f5e9d71d80846a15b52f87bd63ad62feb30a1e05229cf3783bae.

Evidence remains in out/cpu-lifecycle-integration-v047, with isolated builds in out/build/cpu-lifecycle-integration-v047. Preparation README/ready files intentionally retain their pre-execution state; renderer-runs and verification.json hold the final executed results. Original experiments and the guarded performance binary are unchanged. All owned probes exited; no game, remote devices, saves, settings or wake requests were used. No merge, publication or speedup claim.

Follow-up: build an independent normal-game candidate with only the accepted memory preflight plus this lifecycle correction, then verify bounded scene transitions using an independent save and normal timing before Android validation. Keep the indexed-window performance change separate until its remaining correctness/device checks are complete. These tests do not establish in-game reachability of the stale sequence or complete concurrency/GPU-lifetime safety.
