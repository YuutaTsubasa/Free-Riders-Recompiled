# Physical allocation search implementation plan

Goal: reduce repeated reservation searches during Loading while preserving
exact top-down allocation addresses, bounds, retention and protection rules.

Evidence: the Thor loading sample attributes about 6% of guest-12 CPU leaves
to available/lowest_conflict and about 9% inclusive to physical allocation.
A private allocation probe will quantify search loops and elapsed time.

Design: add a read-only top-down free-range query to GuestMemory. Walk its
already sorted, non-overlapping reservation vector backwards once. Retain
whole-host-page conflict semantics, exclusive guest execution for allocation,
and all existing commit/pending-write checks. PhysicalMemory uses this query
only after its existing retained-memory and budget checks. Do not introduce
a cached free list or alter allocation order; those add invalidation risks.

- [x] Capture allocation loop counts and time on Thor.
- [x] Add tests comparing the new query to a page-by-page oracle, including
      holes, partial reservation tails, releases, bounded windows and 4GB ends.
- [x] Implement query and integrate; run guest/physical memory tests.
- [x] Benchmark old and new search on identical synthetic reservation maps.
- [x] Run the matched device fixture with reference verification, then a normal
      APK for stability verification. Do not claim FPS from tracing runs.
- [x] Restore normal APK, preserve saves/settings, record actual limitations.

Outcome: 3,569 live fresh-allocation queries agree exactly, with query time
414 ms old versus 46 ms new. Normal and verifier fixtures both reach 14,400
presents. Android/Windows builds and relevant host/device tests pass. Source
review found no actionable issue. The normal APK is installed, debug.env is
removed, and the launcher is open. Retain the documented large-request search
tradeoff and residual long frames; no end-to-end FPS or Loading gain claimed.
