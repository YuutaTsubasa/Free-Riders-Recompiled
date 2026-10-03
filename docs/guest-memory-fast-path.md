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

Measured on an i5-3470 with an RX 480 (D3D12, Windows 10). Each run is one
unattended Free Race with the benchmark harness. Six interleaved rounds, plus
one uncounted warm-up. The fps is taken over the same stretch of race time
(seconds 20 to 75), so a faster build does not get a different part of the
race.

| | plain | fast path | change | per-round ratio | faster rounds |
| --- | ---: | ---: | ---: | --- | ---: |
| fps, race seconds 20–75 | 24.9 | 27.5 | **+11%** | 1.19 1.16 1.04 1.17 1.06 1.07 | 6/6 |

- The plain build's own spread was 8% (23.7–25.8 fps). The gain was there in
  all six rounds, with a median of +12%.
- Main-thread permit hold per frame went from 25.8 to 22.2 ms. Frames over
  50 ms went from 252 to 48 (median per run).
- All 13 runs ended normally at their present limit, with no hang report.
- **The executable grows from 114 MB to 162 MB**: each access now carries its
  inlined fast path.

Both executables came from the same fork build, with
`SFR_PARALLEL_WORKER=cores`. That build is v0.4.7 plus the fork's own
render-thread work: draw constants staged and whole draws deferred to the
render thread, DEC3N vertices repacked there, and unchanged index buffers kept.
The two executables differ only in `SFR_FAST_PATH();`, which was added to the
same generated sources by a post-processor. This exact branch, on plain v0.4.7,
has not been timed yet. The Linux build of `sfr_memory_test`
passes, and so do the Python generator tests. Not yet run: Android/AArch64,
Vulkan, and the handhelds.
