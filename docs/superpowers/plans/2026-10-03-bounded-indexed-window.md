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

The guarded private candidate is built, reviewed and locally smoke-tested. It has not been promoted to production or measured on Ally X/Android. The prior unguarded candidate's 6.5% main-CPU reduction does not establish this revised build's performance.

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
