# Guest memory fast path at function entry

Every scalar guest load and store in the generated code goes through
`GuestMemory::load`/`store` on `sfr::active_memory`. The build has no strict
aliasing, so after each guest store the compiler must assume that
`active_memory`, the fast page table pointer, the pinned-page table and the
base may have changed, and loads them again before the next access.

`scripts/generate_diagnostic.py` now emits `SFR_FAST_PATH();` after each
function's `PPC_FUNC_PROLOGUE();`. It copies what every access reads from the
`GuestMemory` into a local `GuestMemory::FastPath` once per call (the object,
the fast page table, the pinned-page table and the reservation owner). The
`PPC_LOAD_*`/`PPC_STORE_*` macros then use that copy and the function's own
`base` argument (which is `base()`).

The decision at each access is the same as in the members:

- a load is direct only on a `fast_access` page, inside one page;
- a store is direct only on a `fast_access` page, inside one page, with no
  live reservation of this thread and the page not pinned; a watched page
  still records the write;
- everything else, including `fast_special` pages, page-crossing words,
  import variables, computed words and unmapped addresses, calls the member
  `load`/`store` and behaves exactly as before.

The page table entries and pin counts are still read at each access (relaxed
atomics, as in the members). Only pointers that do not change for the lifetime
of the `GuestMemory` are cached. Code without `SFR_FAST_PATH();` (any
hand-written function using the macros) sees a namespace-scope `sfr_fast` of
type `NoFastPath`, and its accesses are the member calls they always were.

`guest_memory_test` checks the fast path against the members on ordinary
words, a word across pages, computed words, import variables, unmapped
addresses, watched pages, a live reservation and a pinned page.

## Evidence and limits

Measured on an i5-3470 with an RX 480 (D3D12, Windows 10,
`SFR_PARALLEL_WORKER=cores`). Each run is one unattended Free Race with the
benchmark harness. Six interleaved rounds, plus one uncounted warm-up. The fps
is taken over the same stretch of race time (seconds 20 to 75), so a faster
build does not get a different part of the race.

Both executables were built from this branch on v0.4.7. The fast-path build
uses the generator's output as is; the plain build uses the same output with
every `SFR_FAST_PATH();` line removed, which is byte-identical to what the
generator produced before this change.

| | plain | fast path | change | per-round ratio | faster rounds |
| --- | ---: | ---: | ---: | --- | ---: |
| fps, race seconds 20–75 | 25.6 | 29.4 | **+15%** | 1.13 1.13 1.18 1.16 1.16 1.19 | 6/6 |

- The plain build's own spread was 5% (24.8–26.1 fps). The gain was there in
  all six rounds, with a median of +16%.
- Main-thread permit hold per frame went from 23.0 to 19.7 ms, P95 frame time
  from 49.3 to 42.1 ms, and frames over 50 ms from 149 to 22 (median per run).
- All 13 runs ended normally at their present limit.
- **The executable grows from 114 MB to 162 MB**: each access now carries its
  inlined fast path.

An earlier measurement on a fork build (v0.4.7 plus render-thread changes, the
same marker-only difference) gave 24.9 to 27.5 fps, +11%, also in all six
rounds.

The Linux build of `sfr_memory_test` passes, and so do the Python generator
tests. Not yet run: Android/AArch64, Vulkan, and the handhelds.
