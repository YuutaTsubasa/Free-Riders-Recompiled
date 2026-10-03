# Sparse vertex gather observation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development for implementation and sequential scope/quality review.

**Goal:** Measure whether the remaining sparse-gather CPU hotspot repeatedly produces unchanged geometry, before proposing any reuse optimization.

**Architecture:** Private diagnostic build only, retaining the accepted ordinary-page preflight and original worker/timing/rendering behavior. Observe the already-produced gathered byte span; never bypass, cache or change a draw. Reuse the reviewed bounded independent-save launcher after source/binary provenance and reviews pass.

**Tech stack:** C++20 probe helper, frozen graphics-hook source, existing clang/lld recipes, Python verification and bounded Windows runner.

This is evidence gathering under the approved main-thread investigation, not a new production cache design. Fresh Ally Vulkan attribution records 1,304 indexed-hook leaf samples, concentrated in the already-inlined gather loops. Inclusive indexed-draw samples are 32.19% of main samples and cannot be treated as removable time.

Alternatives: increasing the compact-range threshold would change upload workload without knowing its cost; adding a cache now would require unproven index/vertex alias/remapping invalidation. Instead collect distribution and sampled output fingerprints first. Fingerprint matches are observations, not a proof that future guest reads may be omitted.

## Private probe

- [x] Test a standalone helper before inserting it into a copied graphics hook. Cover identical/different observations, key distinctions, count/byte limits, reset/flush boundaries and input immutability.
- [x] Opt in with `SFR_SPARSE_GATHER_PROBE=1`; no production source/header modifications. Restrict draw observation to the half-open present range `[13200,16800)`, flushing the final summary at boundary16800. Count sparse gather calls/output bytes/source-span bytes and stride/cut distribution. A call is observed only after the existing gather has produced its bytes, before the existing `native_draw` call.
- [x] On sampled frames (every60 presents), fingerprint at most8MiB of already-gathered bytes per frame and retain at most4096 key/digest entries. Key identifies original index range/count/width/base, stream physical address/size/stride and primitive/restart context. Count same-key repeated and matching/changing fingerprints explicitly, separating same-frame and cross-frame observations, including unsampled calls, whole spans skipped for the byte budget and capacity exhaustion. No guest memory reads, epoch/watch manipulation or inferred cache hits.
- [x] Summarize bounded counters at present boundaries, including an observation-window end flush. No per-draw logging or unbounded memory. Measure no performance gain from this instrumented run.
- [x] Reproduce the frozen baseline hook object (allow only COFF timestamp difference), compile the private hook, replace only that object in the accepted-memory link recipe, and retain source diff, all input hashes and final executable/map hashes.
- [x] Sequential independent spec and quality review before any game launch.

## Capture and disposition

- [x] Primary launches one bounded local Windows Vulkan scene first, with independent fixture/save/cache, normal game/UI/audio timing and complete rendering. Preserve process identity, package/source hashes, screenshots and cleanup proof. Do not touch Android or other applications.
- [x] Analyze actual sampled workload and coverage/exclusions. Stable fingerprints only motivate a subsequent invalidation study; do not promote this diagnostic to production or claim a speedup.
- [x] Update handoff, stop all owned processes/wake requests, preserve normal settings. No push, merge, release or external message.

## Observed result (2026-10-03)

One stationary Dolphin Resort scene on local RTX 4090 Vulkan completed the bounded run in 275.719 seconds. The private executable SHA256 is `4348116d61d7ec1a39d730c74eb9906486b37b8e76d2ad877292249af4c2e889`; raw log SHA256 is `1b63351e62c759f0a58e1d78c4076867ebda192fb831905c0f879d78c876d3b0`. Evidence is under `out/cpu-gather-v047/local-vulkan-observation/`.

All 60 cumulative summaries passed offline boundary, capacity and conservation checks. Across 3,600 presents, 345,600 gathers produced 13,192,228,800 bytes: 96 calls and 3,664,508 bytes (3.49 MiB) per present. The summed source spans averaged 47,816,688 bytes per present; overlapping ranges mean this is not a unique memory footprint.

The 60 sampled presents covered 5,760 fingerprint observations and 219,870,480 output bytes (1.67% of total gathered bytes). There were 95 retained keys, 5,605 cross-frame matching fingerprints and 60 same-frame matches, with no observed changes. No sampled spans were excluded by the byte or key caps. All observed gathers had primitive restart disabled.

These are fingerprint observations in one stationary scene, not byte-equality, cache-validity, moving-race coverage, an Ally result or a measured speedup. Periodic sampling can miss intervening writes. No runtime optimization was promoted from this experiment.

Screenshots at presents 14,400 and 16,800 show the complete race scene and HUD. HUD time advanced 27.12 seconds versus 27.111639 seconds between image timestamps. This supports normal game-clock progression, not audio quality or pixel equivalence. The owned game exited at the configured present limit; cleanup reported no errors, released the temporary wake request and preserved source save hashes. A fresh process check found no diagnostic game.

Next investigation should characterize individual gather spans and reuse eligibility before choosing between the existing indexed-buffer path and a new cache. Any reuse requires validated index/vertex writes, aliases, remapping and restart behavior; these fingerprints do not establish that contract. Avoid blindly widening the indexed-upload threshold from aggregate byte ratios.
