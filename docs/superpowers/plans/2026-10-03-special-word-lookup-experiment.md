# Private special-word lookup experiment

Status: separate follow-up within the approved main-thread CPU investigation. No production change or integration until measured and reviewed.

## Evidence

In the exact-map Windows sample, main TID 29956 has 553/23,940 samples in `special_word` (2.31%). Of these, 508 samples (2.12% of main samples) fall within its scan instructions. Concrete callers include generated scalar float loads. This is an attribution ceiling, not an expected speedup; actual registry sizes were not recorded and a replacement lookup still costs CPU.

## Contract and smallest candidate

Keep the existing `special_words_` vector ordered by address when adding imports or computed words, under the existing exclusive layout lock. Under the existing `read_layout()` guard, find the first word with `uint64_t(word) + 4 > address`, then check whether its start precedes `address + size`. Only the boolean membership index changes. Do not sort `variables_` or `read_only_words_`, change provider invocation or error precedence, add metadata or caches, remove locks, or change fast-page admission/publication. Preserve unaligned imports and accesses near the top of the address space.

Build this against the original runtime, without the memory-preflight or worker-wait changes. Keep the exact baseline timing object and all other link inputs. Tests are local, private, and bounded; primary alone owns device experiments.

## Gates

- [x] Establish baseline differential snapshots before building the private candidate.
- [x] Compare complete values, provider counts, bytes and RuntimeStop details for descending/random registration, unaligned import words, adjacency/overlap edges, scalar sizes 1/2/4/8, page boundaries and top-of-address-space access. All 8,376 observations and 14,400 synchronized concurrent reads match.
- [x] Run the existing guest-memory, pending-write and vector-memory suites against both variants.
- [x] Verify exact source/object/executable hashes and only one runtime object replaced; separate scope and correctness reviews passed for a private device experiment.
- [ ] Only then perform matched direct-file Ally X tests with independent saves, normal game time and full rendering. Require repeatable benefit beyond control drift without tail regressions.

If no measurable gain remains, leave production unchanged. Do not publish, merge or claim Android benefit from this Windows experiment.

The private executable SHA256 is `aaa798d7f6c114c76ec4809b1ea02aff772cb3519a6536a900854a1aeaf24ab8`. It retains the original preflight, worker budget and timing object, even though the separate preflight change has since been accepted into the branch. Compare it only with the preserved original baseline; do not describe it as a combined optimization.
