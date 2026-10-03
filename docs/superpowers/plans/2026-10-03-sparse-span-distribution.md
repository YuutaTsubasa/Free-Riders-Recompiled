# Sparse gather span distribution

> **For agentic workers:** Use superpowers:subagent-driven-development, with sequential scope and quality reviews before runtime capture.

**Goal:** Measure which remaining gather calls could meet a wider existing GPU-indexed window threshold, before changing any draw behavior.

**Scope:** Private observation under the approved main-thread investigation. Production remains the accepted memory preflight only. The private lifecycle experiment is not linked. No cache, altered draw, new watch, skipped work or performance claim.

The first probe measured repeated gathered output but only aggregate source spans. Those spans can overlap and host repacking can enlarge uploads, so their sum does not establish a suitable threshold or cache budget. This probe records metadata already calculated by the gather path; it does not inspect guest bytes or fingerprint output again.

## Implementation and capture

- [x] Test a fixed-storage helper that observes only presents `[13200,16800)`. Partition every gathered draw exclusively into restart, source span over 8 MiB, or non-restart in-cap ratios `<=4`, `(4,8]`, `(8,16]`, `(16,32]`, `(32,64]`, `>64`. Ratio compares source-span bytes with gathered-output bytes using overflow-safe arithmetic. Count calls, gathered bytes, source-span bytes, original index bytes and decoded 32-bit GPU-index bytes per bin; retain exact strides through 256 and one overflow bucket. Validate zero inputs, cutoff equality, large values, partition conservation and final flush without draws.
- [x] Copy the frozen original graphics hook and add opt-in `SFR_SPARSE_SPAN_PROBE=1` metadata observation after the existing gather, plus bounded cumulative summaries at 60-present boundaries. Emit stride detail only in the final summary. Do not alter existing gather/draw arguments, memory checks, indices or timing.
- [x] Reproduce the baseline hook object with only COFF timestamp differences allowed; replace only that object in the accepted-memory executable link. Preserve all 202 object and 18 library identities, diff, source/helper/executable/map hashes. Keep old probe artifacts unchanged.
- [x] Complete independent scope review then quality review. Package only the new runtime into a copy of the verified normal test ZIP. Derive the bounded local Vulkan launcher from the earlier reviewed probe launcher, changing only probe name, private paths, hashes and descriptions.
- [x] Run once with independent save/cache and existing 400-second engine / 420-second controller bounds. Preserve source fixture hashes, owned process identity, final-window summaries, clock screenshots and cleanup. No Ally/Android operations in this local characterization.
- [x] Verify counter partitions, final boundary and clock progression; report cumulative eligibility at 8/16/32/64 separately from total source upload bytes. These are raw-span estimates, not actual host repacking size, unique resident memory, GPU cost, cache safety or a speedup. Keep production threshold unchanged until an independently justified A/B.

Artifacts: `out/cpu-span-v047`, `out/build/cpu-span-v047`. Do not reuse completed capture paths or modify prior experiments.

## Result (local RTX 4090 Vulkan, 2026-10-03)

All 60 summaries passed accounting checks over 3,600 presents. The 345,600 gathers produced 13,192,228,800 output bytes from summed source spans of 172,140,076,800 bytes. No invalid metadata, restart, over-8-MiB or already-eligible `<=4` gather was observed. Exact gather strides were 16, 20, 24, 28, 32, 56 and 60.

| Hypothetical threshold | Newly eligible calls/present | Share of remaining gathered bytes | Extra uncached raw bytes/present |
| --- | ---: | ---: | ---: |
| 8x | 26 | 53.50% | 8,567,036 |
| 16x | 52 | 80.89% | 18,021,168 |
| 32x | 69 | 96.71% | 30,400,976 |
| 64x | 80 | 98.65% | 33,985,144 |

At 8x, the affected output averages 1,960,472 bytes per present. Its checked source spans total 10,235,528 bytes, plus 291,980 bytes of decoded 32-bit GPU indices. The table subtracts original gathered output from those hypothetical raw uploads; it does not measure actual upload traffic. Repacking, alignment, overlap, cache hits and resource lifetimes are not included. The 53.50% figure is a byte share, not a CPU-time saving.

This supports 8x as a narrower candidate for a controlled experiment, not an optimum or a production change. Compare against the **accepted-memory 4x baseline**, using the same API/scene and direct-file harness; do not use the original pre-memory executable as its control. Keep the 8 MiB cap, restart exclusion, all input checks and draw ordering. Measure main CPU/frame, FPS, frame-time tails, gathered/uploaded/cached bytes, draw/index counts and clock correctness, including cold preparation cost. The additional traffic may outweigh saved CPU work on handheld devices. Do not combine a threshold experiment with the private lifecycle patch or a new cache.

The run exited at its 18,001-present bound after 275.766 seconds. Screenshots show the stationary Dolphin Resort scene; HUD time advanced from 29.86 to 57.20 seconds, versus 27.334328 seconds between screenshot timestamps. Cleanup preserved source save hashes, stopped the owned game and released its temporary wake request. This is not a lap, audio-quality, pixel-equivalence or performance test. Independent raw-log/calculation/visual review passed.

- EXE SHA256: `0e7a456d612c435fb6a4648e63a5596bdcc477a00b00d369f576c398d9f63f92`
- Private ZIP SHA256: `6718b9f342a242c6469860721322480fab7b910638f015fc4c75e2ea1437c07e`
- Raw log SHA256: `880e7e281db421b62fcc2b996d47dab5d7bcabbbaa5e824a9ced225eaa65e650`

No threshold change, cache implementation, device installation, push or release occurred. Production remains unchanged beyond the previously accepted memory preflight.
