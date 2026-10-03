# Bounded indexed-window extension experiment

> **For agentic workers:** Use superpowers:subagent-driven-development with sequential scope and quality review.

**Goal:** Preserve the measured 8x opportunity while ensuring newly admitted draws fit the existing uncached host upload ring, without changing the old 4x route.

**Authorization:** This is a private correctness prerequisite within the user-approved common main-thread CPU work. No production promotion, merge, release, external messages, or Android changes.

**Design:** Keep the existing non-restart condition and 8 MiB raw-span cap. Admit the old `block <= 4*order.size()` cases exactly as before. Admit additional `block <= 8*order.size()` cases only when `block <= 0x40000` (262144 vertices). Native draw accepts at most 16 declaration elements; each DEC3N adds 8 bytes per vertex. Newly eligible host vertices are therefore at most 8 MiB + 262144*128 = 40 MiB. Since these draws fail the 4x condition, their GPU index count is below block/4, so uint32 index data is below 256 KiB. Alignment, constants and the maximum 16 KiB palette leave the total well below the existing 64 MiB ring. Use 64-bit arithmetic and verify the actual renderer formula. This guard is conservative and adds no declaration reads or cache-state changes.

**Alternatives considered:** Exact layout-aware budgeting admits more spans but duplicates declaration work or changes native_draw responsibilities on the hot path. Lowering the raw cap alone does not bound format expansion at small strides. The conservative vertex-count bound is simplest for this private follow-up; it does not fix any pre-existing oversized case already admitted by 4x.

**Files:** New `out/cpu-index8-bound-v047` test/build helpers and `out/build/cpu-index8-bound-v047` outputs only. Preserve all previous artifacts. The frozen accepted-memory benchmark source and all other 201 objects/18 libraries remain unchanged. No probes or private lifecycle patch.

- [x] Write a regression oracle using the actual extracted source predicate and an independent host-upload size calculation. Show unguarded 8x admits the concrete 72 MiB case while it previously gathered into 9 MiB; assert the desired safe-extension behavior and preserve the red result.
- [x] Build the private minimal guarded predicate. Confirm exact old-4x decision preservation, new eligibility boundaries around ratios 4/8, raw 8 MiB and block 262144; restart handling remains unchanged. Cover strides, 0..16 DEC3N elements, 16/32-bit guest indices, index/base-vertex sequence equivalence, and deterministic randomized cases. Do not mistake predicate tests for complete renderer/GPU/cache tests.
- [x] Reproduce original hook object except timestamp, replace only the hook in the accepted-memory link, retain 202-object/18-library hashes and new map/RVA. No game/device actions in the build task.
- [x] Sequential independent scope and quality reviews; primary reruns the reviewed regression oracle and validates provenance.
- [x] Record the exact revised candidate and remaining limitations. Runtime verification and a repeated performance comparison need fresh cases and hashes; do not pool prior unguarded results into a claim for this revised candidate.

No change to game/UI/audio time, required rendering, draw order, indices/base vertex, guest checks, original worker wait budget or vertex-cache lifetime. No fixed FPS claim before revised runtime measurements. Scope of the size guarantee is only the additional route admissions beyond the original 4x behavior.

## Bounded local runtime follow-up

After both candidate reviews pass, primary may use `out/cpu-index8-bound-v047/prepare-local.py` to derive and inspect two launchers from the previously reviewed normal-package smoke. Only the private executable changes in the payload; the new launchers explicitly label the benchmark hook, enable metrics, and use separate save/cache directories.

- [x] Inspect generated diffs and package CRC/payload hashes, then run Vulkan Dolphin Resort and D3D12 Frozen Forest sequentially (not concurrently). Stop at present 15601; engine watchdog 400 seconds, outer timeout 420 seconds; capture frames 14400 and 15600. Each launcher refuses existing games and releases its temporary wake request in cleanup.
- [x] Verify bounded exit, full scene and normal clock from screenshots/logs, original fixture hashes and process/wake cleanup. This covers stationary starts only, not a Frozen Forest water-channel traversal or complete race. Do not convert these uncapped liveness smokes into an FPS improvement claim.


## Verified result (2026-10-03)

The guarded private candidate is built, reviewed and locally smoke-tested. It has not been promoted to production. At this local-smoke checkpoint it had not been measured on Ally X/Android; the fresh Ally follow-up is recorded below. The prior unguarded candidate's 6.5% main-CPU reduction does not establish this revised build's performance.

The actual extracted unguarded predicate fails the upload-budget oracle: the concrete 72 MiB expanded window needs 75,784,960 bytes including indices and constants. RED reports 2,400 failed assertions among 76,546 checks. The guarded predicate rejects that new-route case; GREEN reports 68,046 checks, zero failures and 267 additional safe admissions. Legacy 4x decisions, including the explicit legacy oversized case, remain unchanged. Primary reran the frozen green executable with a 45-second timeout and obtained the same successful result. CPU decoder/primitive checks use the actual pinned native_formats archive member. These tests are not GPU/cache/image equivalence evidence.

Scope and quality reviewers independently passed the source, proof, tests, link inputs and smoke emitter. The hook source reproduces the original object except COFF timestamp bytes, and the candidate replaces only one object among 202 objects and 18 libraries, retaining accepted memory and original worker/timing behavior.

- Candidate source SHA256: `2232f5d2df886e4a0f35df774d163dd5666e5349da8fb0b2b7db611106f3c94a`.
- EXE SHA256: `46d7a42a721180053b7abcc2745b22cbbd95986d47c7a72655b6c8a2c100788a`.
- MAP SHA256: `2be6410229d15b8d9f4bc1ac2881d7528e96f6ed9adff4411dc55471576f2ff3`; process-start RVA `0x748b5e0`.
- Private smoke ZIP SHA256: `63c97c38f654d79b61440e156aeb832fe2df4771e06aac556b8bebc7ea8385c2`. All other payload entries match the previously verified normal memory package.

Sequential RTX 4090 local smokes completed:

| Backend / stationary start | Runtime | Exit | HUD elapsed | Logged elapsed |
| --- | ---: | --- | ---: | ---: |
| Vulkan / Dolphin Resort | 248.125 s | present limit 15601 | 13.29 s | 13.2848 s |
| D3D12 / Frozen Forest | 241.672 s | present limit 15601 | 9.47 s | 9.4689 s |

Primary inspected both screenshots per case and verified 1,201 contiguous racing/presented records between frames 14400 and 15600. The rider, scenery and HUD were present. Both used the exact candidate and correct RTX 4090 backend, retained original fixture hashes, exited normally without forced timeout, stopped their owned processes and released temporary wake requests. No cleanup errors occurred. Ordinary launcher settings were never edited. Independent final runtime-evidence review is recorded in the continuation.

Remaining limits: stationary starts are not whole-course, Frozen Forest water-channel, audio-quality, cache-lifecycle or pixel-equivalence verification. No Ally X or Android contact occurred in this follow-up. Runtime tests use the private uncapped rendering hook with normal game/UI time, not a user-ready normal binary. No new FPS claim, source promotion, push, merge or release.

Next: use new matched cases and the exact guarded candidate to verify repeatability on Ally X, including both controls and fresh per-case app caches. Treat driver-cache warmth separately and do not reset shared driver caches. Preserve this candidate/hash; any further change needs a new artifact and measurement. Broader scene/cache and Android validation remain before production acceptance. Avoid rerunning these completed local starts without a new concern.

## Fresh Ally X guarded-candidate comparison

Use new cases `directlog-index8bound-base-y`, `directlog-index8bound-candidate-z`, and `directlog-index8bound-base-aa`, in that order. Controls use accepted-memory EXE `35b701374f0f85f353f85e767c229bdc62bb6662d0b6723a047863c5b739a403`; candidate uses the guarded EXE recorded above. This is a fresh comparison, not pooled with unguarded V/W/X.

- [x] Derive new reviewed direct-file harness mappings, verify source/map hashes and generated environment, then scope and quality review before device use.
- [x] Check Ally idle once, deploy to a unique verified filename, run sequential bounded cases with independent saves/app caches and temporary wake request. Stop dispatch on failure; do not reset shared driver caches.
- [x] Compare CPU per frame, FPS, tails, gather/repack/cache metrics and original draw/index counts. Inspect screenshots and normal game time. Report first-executable pipeline startup separately.
- [x] Stop owned tests, release temporary wake, remove owned completed tasks, verify original fixture and ordinary state. Record limitations and leave production unchanged pending broader coverage.


## Fresh guarded Ally result (2026-10-03, Y/Z/AA)

The reviewed harness and analyzer passed scope and quality review; primary reran the offline mocked sequence and analyzer self-tests. All three Vulkan stationary Dolphin Resort cases completed normally with exit 3 and `STOP benchmark-window`. Each used independent saves/app caches, the exact pinned EXE/map, original worker budget and normal game/UI time. The fresh guarded candidate replaces only the indexed-window predicate. No old unguarded controls were pooled.

| Metric | Control Y | Guarded Z | Control AA |
| --- | ---: | ---: | ---: |
| Main CPU ms/frame | 9.704650 | 9.194413 | 9.820203 |
| Process CPU ms/frame | 19.763597 | 19.229230 | 19.223974 |
| FPS | 71.374346 | 73.564787 | 72.714436 |
| Frame P99 ms | 17.402580 | 16.295625 | 16.563940 |
| Maximum frame ms | 109.3520 | 27.8893 | 35.4916 |
| Contiguous measurement frames | 7137 | 7356 | 7272 |

The candidate main-CPU estimate is 5.2577% and 6.3725% lower than the two separate controls; their main-CPU drift is 1.1907%. FPS is 3.0689% and 1.1694% higher, while control FPS itself drifts 1.8776%. This supports a promising main-thread saving in this scene; the smaller FPS difference needs caution. Process CPU is 2.7038% lower than Y and effectively equal to AA (0.0273% higher), so there is no consistent whole-process CPU or power saving claim. One guarded candidate run does not establish statistical significance or general improvement across hardware/scenes.

Gather costs are 0.677863/0.335694/0.678283 ms/frame, with repack 0.293067/0.146702/0.293374. This matches the intended reduction in repeated vertex preparation. Mean guest draw counts are 680.432/676.087/677.854 and original guest indices 854053/852248/854405; animation/timing means these are not frame-identical replays. No frame gaps, upload ring flushes or texture upload budget drains occurred in the measurement windows. The candidate's path-dependent vertex span increases about 8.25 MB/frame and cached span about 10.21 MB/frame, while swap/repack bytes decrease. These overlapping counters are not measured PCIe traffic, GPU residency or total memory consumption.

Candidate first-executable pipeline preparation took about 47.473 seconds for 336 recipes. Fresh per-case app caches do not imply cold driver caches; shared driver caches were not reset. Report startup separately and do not attribute that difference solely to this predicate.

Primary viewed all six BMPs. HUD elapsed times for Y/Z/AA were 66.91/65.85/65.93 seconds, versus logged 66.9043/65.8695/65.9415 seconds; all agree within 20 ms of display/frame quantization. Complete stationary scenery, rider and HUD were visible. This is not pixel-equivalence, completed-lap, water-channel or audio-quality evidence. Independent final measurement review passed: raw CPU/QPC/TID computations, all frame/metric statistics, 42 input hashes, all six images and cleanup evidence were reproduced. The reviewer agrees with the private-only disposition and stated limits.

Batch session 31859 exited zero. Wake session 51565 explicitly released its request and exited zero before its 30-minute deadline. Final remote snapshot at 04:31:05 UTC found no owned games/tasks/wake, the original Turbo power plan, and original source-fixture hashes matching both the preflight and local source. Ordinary launcher settings were not edited. Android was not accessed.

Evidence: `out/cpu-index8-bound-v047/analysis-results/results.json` SHA256 `5899aa26304c78927d7ea1dc937863c8dc1d174f059a26b060efa659f6dd40b6`; `visual-clock-verification.json` SHA256 `8297984323e6a06fe90d4b358af38c021b34082bfdda94c7058b54870a526b74`; `device-cleanup.json` SHA256 `6f60d426e57c16b39453fd76027af58cd125e971b439b798084455566da7fa21`. The generic analyzer report's pending-visual label is superseded by the separate visual proof, not silently rewritten.

Disposition: retain the frozen guarded candidate privately for broader validation. Production still uses 4x; only the already accepted memory-preflight optimization is present. Do not rerun this completed stationary batch without a new hypothesis. Next useful work is a distinct scene/cache-lifetime correctness check and then shared-code Android validation with a new artifact, preserving ordinary app/save state. No merge, release, push or external comment.
