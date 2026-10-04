# Ordinary-page write preflight experiment

Status: private investigation within the approved CPU follow-up. Do not combine with the worker waiting experiment or promote without measurements and review.

## Evidence and hypothesis

The exact-map 35-second WPR capture records `GuestMemory::check_store_access` in 1.77% of main-thread leaf samples and `special_word` in 2.31%. Graphics state and texture preflight repeatedly check small ordinary memory ranges. These sampled costs are not an additive or guaranteed speedup estimate.

The existing page flags already distinguish complete ordinary pages from partial, unmapped, import and provider pages. Test whether a whole-range, single-page fast-access check can avoid the general validation scans. Prefer reuse of existing metadata over another cache, registry sorting or a new concurrency protocol.

## Contract

At the beginning of `check_store_access`, only with a null pending-write owner, accept an existing `fast_access` whole page when `page_pinned(address)` is false. Call **the unchanged `mark_written(address, size)`** before returning. Leave every other path unchanged.

Do not use `fast_write`: its partial-page and reservation rules are different. Do not reject a live reservation here: reserved operations also call this preflight, and `check_write` retains its later reservation rejection. Preserve dirty bits, epochs and the watched-write counter, including marking before a reservation error. Owner publication and any pinned page always use the original validation and overlap precedence. `complete_store` and write-combined fences remain unchanged. Existing exclusive layout mutation/pin admission requirements still apply; this is not a new guarantee for concurrent remapping.

## Verification and gate

- [x] Build a differential contract test against the unmodified source first. Record successful baseline output.
- [x] Build a private copied source with only this early return and compare complete observable output: all 1,208 snapshots match, including bytes, dirty/epoch state, counters, provider calls and full RuntimeStop details.
- [x] Cover complete ordinary ranges and top-of-address-space accesses; zero, overflow, cross-page, partial, unmapped, decommit and release cases; import/provider behavior; pending leases and disjoint same-page writes; word/doubleword reservations and failed conditional writes.
- [x] Existing guest memory, pending-write and vector-memory suites pass against both variants. The 202 retained object and 18 local library hashes were checked; the old build cache is unchanged.
- [x] Separate scope and correctness reviews found no blocker for an isolated game experiment. Serial `concurrent_reader` coverage does not establish new concurrent admission guarantees.
- [ ] Link a private diagnostic with only the guest-memory object replaced, retaining the original worker budget and timing harness. Compare matched Ally X runs with independent saves and bounded normal-time races.
- [ ] Accept only a repeatable CPU/main-thread benefit exceeding run drift, without new errors, rendering omissions, altered game time or frame-pacing regression. Otherwise retain production code and document the negative result.

No release, merge, external comment or Android performance claim is part of this experiment. A microbenchmark may diagnose local cost but cannot establish gameplay improvement.
