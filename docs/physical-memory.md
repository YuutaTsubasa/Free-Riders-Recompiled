# Bounded native physical allocation

Original wrapper `sub_824DF2B8` calls `__imp__MmAllocatePhysicalMemoryEx` at `0x82ACBA0C` and returns to `0x824DF314`. The observed six arguments are flags0, size0x07203000, protection4, physical minimum0, physical maximum0xFFFFFFFF and alignment4. The original wrapper checks for a null result and sets error8 on failure.

## Address and backing model

The [pinned Xenia allocation implementation](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc#L333) defines these six32-bit arguments, rounded allocation, inclusive physical range limits and pointer/null return. Its [memory layout](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/memory.cc#L157) supplies the4KiB view beginning0xE0000000 with size0x1FD00000. The [view mapping](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/memory.cc#L245) places a0x1000 offset between this view and raw physical memory.

The bounded native allocator uses that view, with physical address = virtual address −0xE0000000 +0x1000. Its allocations are actual zeroed, writable GuestMemory mappings. Guest physical contiguity is an address-space contract; it does not imply that Windows allocated contiguous physical RAM. A later native graphics backend must consume this same backing through its own resource handling.

Only flags0, read/write protection4 or Windows write-combined protection404, default4KiB pages and alignment0 or a power of two no larger than4096 are supported. Alignment rounds to4096. Other page sizes, cache flags and allocation classes stop before mutation. Protection404 uses [actual Windows write-combined backing](write-combine.md), including checked-store ordering and unsupported atomic-access checks. The constructor requires4KiB-compatible usage granularity; no other platform support is claimed here.

Sizes round upward in64-bit arithmetic. The entire rounded allocation must fit the intersection of requested inclusive physical bounds with the known arena. Candidates are searched downward at4KiB alignment and checked against actual GuestMemory reservations, so externally occupied ranges are respected. Empty bounds, zero/oversized requests, unavailable space or insufficient native backing budget return null without changing the memory registry. Unexpected host allocation failures remain explicit stops under the existing reserve/commit behavior.

Only this one view is mapped. The64KiB and16MiB aliases, raw GPU view, freeing and other physical-memory imports are not silently provided. Access through an unimplemented view fails the existing checked-memory boundary. Adding aliases later requires shared backing and deduplicated budget accounting; independently copying memory would be incorrect.

## Runtime and statistics

The exact name/address handler runs after the reservation guard. It logs all six original argument values, invokes the real allocator and changes only r3 to the zero-extended pointer or null. The game remains responsible for testing the return and using the region.

MemoryStatistics now counts committed bytes in this arena as title physical pages. These pages reduce available backing and are excluded from kernel metadata. Physical reservations do not increase virtual arena capacity or reserved virtual bytes. Image, stack and TLS category layouts must also be disjoint from the physical arena. Only one backing view exists, so its pages are counted once.

Tests check zeroed/read-write backing, rounded size, descending allocations, physical limits, occupied ranges, exhaustion, unsupported requests, boundary arithmetic and distinct statistics categories. True-entry evidence is required separately; successful allocation is not title/menu acceptance.

## Initial verified entry

The true entry allocated0x07203000 bytes at0xF8AFD000 and continued to1267 function entries. It then requested0x12C0 bytes with protection0x404 and alignment0x20. This separate cache-attribute request stops explicitly at the unsupported protection boundary; it has not been treated as a successful allocation. Last function sub_824DF2B8, LR824DF314, exit3.

Evidence: out/physical-memory-red.log, out/physical-memory-final.log, out/physical-statistics-red.log, out/physical-statistics-green.log, out/physical-memory-boot-red.log, out/physical-memory-build.log, out/physical-memory-build-final.log, out/physical-memory-boot.log, out/physical-memory-tests-final.log (103 tests/no skips) and out/physical-memory-ctest-final.log (16/16). Dependencies verified. Root review corrected a test that originally exhausted budget before reaching its intended occupied-range path; the revised test has sufficient budget and verifies true space exhaustion.

The subsequent [write-combine increment](write-combine.md) implements that second request using actual Windows cache attributes. Current startup reaches VdInitializeEngines after 1,271 function entries; the preceding paragraph records the initial normal-memory checkpoint.
