# Native backing accounting and MmQueryStatistics

The real wrapper `sub_82A70C78` sets size104 at `0x70131510` and calls `MmQueryStatistics` at `0x82ACC44C`, returning to `0x82A70C9C`. It reads total pages, title available pages, virtual capacity and reserved virtual bytes. These values now derive from the native memory manager.

## Accounting model

GuestMemory enforces a default512MiB committed-backing budget. This is a native runtime limit, not sampled host RAM or a claim to emulate Xbox physical page mappings. Reserving the 4GiB host address window does not charge this budget. Committing a logical guest region charges unique host-page-rounded backing; recommits and overlaps do not double count. A budget overflow stops before changing host memory protection or committed-region metadata. Existing reserve-plus-commit operations retain their existing semantics: a failed commit does not roll back a preceding explicit reservation.

The constructor permits an aligned smaller budget for tests and validates host pages/budget before reserving host memory. Usage queries accept aligned bounded ranges, intersect them with the actual reserved/committed extents and count rounded backing even when logical access to a tail or guarded field is prohibited. All reported page counts use4096-byte units.

VirtualMemory has two256MiB allocation arenas. Their actual address capacity and occupied reservations are separate from the backing budget: a reserve-only allocation consumes address capacity but no committed backing. Statistics reflect reservations from GuestMemory across both arenas, including any occupied range, rather than assuming every available address can be committed simultaneously.

## 104-byte result

The field layout and null/size status behavior follow pinned Xenia's [MmQueryStatistics definition](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc#L451). That implementation labels several populated values guessed; those constants are not copied here.

| Byte offset | Field | Native value |
| --- | --- | --- |
| 0 | size | 104 |
| 4 | total physical pages | Enforced backing budget /4096 |
| 8 | kernel pages | Committed backing outside title categories /4096 |
| 12 | title available pages | (Budget − all committed backing) /4096 |
| 16 | title virtual capacity | Actual combined capacity of the two allocation arenas |
| 20 | title reserved virtual bytes | Actual reserved backing ranges inside those arenas |
| 24 | title physical pages | Committed backing in the bounded physical arena /4096 |
| 32 | title stack pages | Known mapped guest stack backing /4096 |
| 36 | title image pages | Known decoded image backing /4096 |
| 44 | title virtual pages | Committed arena backing plus title TLS backing /4096 |
| 100 | highest physical page | Last index in the native backing budget |

Other title categories (pool, independent heap, page table and cache) are zero because those separate native allocation classes do not exist yet. The CRT heap uses the virtual allocator and is counted under virtual pages. There is no separate guest system process, so its11-field section is zero. The bounded physical allocator has its own title physical category; its single backing view is excluded from kernel metadata. Kernel metadata mappings account for the remaining committed backing. This classification must be extended when new allocator classes or physical aliases are added; it is not a full Xbox memory-manager implementation.

Image, stack and TLS category ranges must be fully backed, aligned, mutually disjoint and outside both virtual and physical allocation arenas. Bad layouts stop rather than double count. The production runtime supplies the actual decoded-image size, diagnostic stack range and rounded TLS mapping size after initialization.

## Query boundary

Only exact import name/address `__imp__MmQueryStatistics` / `0x82ACC44C` is handled. Null returns `0xC000000D`; size other than104 returns `0xC0000023` without changing output. The complete valid-sized output must pass writable mapping, import-guard, read-only and reservation checks before any of its26 big-endian words are written. Invalid memory and unsupported accesses remain diagnostic stops. Apart from this output, only r3 receives the status; unknown imports continue stopping.

Tests verify every field, reserved versus committed changes, both arena page sizes, repeated commits, clipped/rounded backing, small-budget exhaustion, output sentinels and invalid layout/size/pointer cases. A valid statistics result is initialization progress, not proof of a title screen or menu input.

## Verified Windows entry

The actual entry now passes this query with budget131072 pages, kernel12, available122627, virtual capacity536870912 bytes, reserved1048576 bytes, stack64 pages, image8352 pages and virtual17 pages. The title totals and kernel pages sum to8445 committed pages, leaving exactly122627 available within the enforced budget. Boot exits3 at `__imp__XGetVideoMode` / `0x82ACB23C`, with last function `__savegprlr_28`, LR `0x8249AEC8`, and1039 function entries. This is two entries beyond the prior stop, with no title screen yet.

Evidence: `out/memory-statistics-red.log`, `out/memory-statistics-green.log`, `out/memory-statistics-boot-red.log`, `out/memory-statistics-build.log`, `out/memory-statistics-boot.log`, `out/memory-statistics-tests-final.log` (103 Python tests, no skips), and `out/memory-statistics-ctest-final.log` (14/14). Pinned dependency verification passed.
