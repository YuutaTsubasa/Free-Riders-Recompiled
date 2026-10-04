# Memory lifecycle epoch experiment

> **For agentic workers:** Use superpowers:subagent-driven-development for the private implementation and sequential scope/quality review.

**Goal:** Establish whether watched-memory lifecycle resets can invalidate future vertex reuse safely, without changing the production build or adding a cache.

**Authorization and scope:** This is a private correctness prerequisite within the approved common-main-thread investigation. User requested autonomous bounded experiments while asleep. No device operations, public changes, or new user configuration are required.

## Evidence and design

The sparse-gather observation found repeated fingerprints, not safe reuse. A standalone audit linked to the accepted GuestMemory object reproduces two counterexamples: decommit followed by commit, and release followed by map, change previously nonzero data to zero while `written_since` still returns false. Unchanged and ordinary-store controls behave as expected. This is a GuestMemory contract observation, not a demonstrated game or renderer failure. GPU-retained allocation reuse already calls `check_write` and must not be described as universally broken.

Alternatives are a new mapping-generation API, refusing reuse for any possibly remapped memory, or conservatively marking watched pages when their backing is discarded. Prefer the last for a private experiment: it reuses existing dirty bits/epochs, introduces no steady-state draw/store work and does not clear watches. This does not by itself solve new-view discovery, raw-pointer writes, index keys or concurrent snapshot consistency for a sparse cache.

Copy the accepted memory source into a new private output directory. Mark each range before the destructive native decommit/release operation, after the existing validation for that operation. A native failure may conservatively invalidate a cache but must not permit stale data after partial backing destruction. Do not mark ordinary recommit of still-committed data or change the existing validation, allocation, protection, locking and reservation rules. Preserve the accepted preflight change and all unrelated source bytes.

## Steps

- [x] Capture the current behavior with unchanged/store controls and decommit/recommit plus release/remap observations. Preserve exact source/object/executable hashes and 30-second test limit.
- [x] Create private contract tests that require reset ranges to report dirty through both epochs and dirty bits. Run against unchanged source and retain expected failures.
- [x] Test partial ranges, adjacent watched pages, watches without epochs, and old/current epoch observers; rejected invalid/pinned/reservation operations must preserve their errors and must not destroy data. Include unchanged recommit and repeated decommit controls.
- [x] Implement only private lifecycle invalidation. Run the new tests and existing guest memory, pending-write, vector memory and physical/virtual memory tests where available. No graphics/game claim from these tests.
- [x] Independently review scope, then correctness and failure ordering; address findings before considering promotion.
- [x] Record results and remaining cache contracts. Do not promote into production solely to pursue a speculative speedup. No device run, merge, release or push in this experiment.

**Artifacts:** `out/cpu-gather-v047/epoch-lifecycle-audit*` preserves the original observation. New private experiment files belong under `out/cpu-lifecycle-v047` and `out/build/cpu-lifecycle-v047`; never overwrite accepted memory or older benchmark artifacts.

## Result and disposition

Completed on Windows, 2026-10-03. The baseline test produced exactly eight stale retained-gather decisions: index and vertex replacement independently, each with 16-bit and 32-bit indices, through both decommit/recommit and release/remap. The candidate passed all 35 cases with zero stale decisions. Primary independently reran the checksum-verified candidate executable with a 45-second bound and obtained the same result. Untouched and ordinary-store controls, rejection preservation, and actual GPU-retained PhysicalMemory reuse remain valid.

Six existing standalone suites passed: behavior, guest memory, pending write, vector memory, physical memory and virtual memory. Five have identical baseline/candidate output. The 1,209-line behavior oracle differs only at three named lifecycle dirty-bit events and their cumulative watched-write counters; all other fields and lines match. Both raw outputs and the strict comparison are retained. This is an intentional extension of content-change notification, not a correction of an API that already promised lifecycle tracking.

The private diff adds four `mark_written` calls immediately before native backing destruction. It retains the accepted preflight and original validation, commit and store behavior. Baseline object reproduction differs only in COFF timestamp bytes 4 and 5. Independent scope review and subsequent quality review both passed.

- Private memory source SHA256: `0b1b15c691963d42a9e81daa6259fbe902078731587681056320e9dabd1f4092`
- Private memory object SHA256: `285b79293bb257c0c0c32a3def0a679b62bdb5f2cf417562dd727ae9225ffba2`
- Regression executable SHA256: `f0a53a3e74f03b1249cf23fb7467b663e7a252bc94366ff2e8d7d1cc720cf3e6`
- Provenance SHA256: `a1a881dc7a1b17585f0cb377d198f3dab4ae0b3848e450ad069e3c0e4791d37e`

**Private only; not promoted.** No cache, game executable, device installation or performance comparison was produced. POSIX execution, induced native failures, concurrent snapshots, epoch wrap, newly visible physical views and rendering resource lifetimes remain outside this validation. The production source and earlier user-test packages are unchanged. Before a sparse cache can be considered, measure potential reuse costs and complete those cache-specific contracts; do not treat this prerequisite as a speedup or complete cache-safety proof.
