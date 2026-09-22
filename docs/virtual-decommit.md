# Native virtual decommit for original heap trimming

The Windows title/menu goal remains **unachieved**. This change implements the
original heap's `NtFreeVirtualMemory` decommit request. It preserves the original
heap bookkeeping and texture creation code.

The [pinned Xenia ABI](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc)
uses two big-endian 32-bit in/out words, an exact `0x4000` decommit mode and zero
debug argument. It returns the unchanged base and size rounded to the selected
guest heap's page size: 4 KiB at `0x10000000`, 64 KiB at `0x40000000`.
Null base returns `C00000A0`; an invalid parameter returns `C000000D`; failure to
find one allocator-owned reservation containing the entire rounded range returns
`C0000001`. Other free modes, debug heaps and unaligned bases stop explicitly.
Zero size is rejected. No release or protection-change operation is added.

Both complete output words must be writable and nonoverlapping before values are
read. Neither may intersect the rounded target, including a partial four-byte
intersection. Foreign mappings and ranges crossing adjacent reservations are
rejected without changing outputs or commitment. Reservation ownership and guest
protection `4` remain unchanged; committed statistics decrease only by the
committed overlap actually discarded.

Windows uses actual `VirtualFree(..., MEM_DECOMMIT)` on owned private backing.
Untouched placeholders stay reserved. Private host allocation extents are retained
independently of logical committed spans, so later `MEM_COMMIT` reaches existing
private reservations as well as newly replaced placeholders. The
[Windows decommit contract](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualfree)
retains address ownership and permits already-uncommitted pages; recommitted
discarded pages are zero while live neighbors retain their bytes.

The operation preflights range ownership, imports, computed read-only words,
write-combined spans and active CPU reservations before host effects. Metadata
splits are prepared first. A host failure after any page discard is fatal: there
is no claim that recommitting could restore the discarded bytes. POSIX uses
anonymous inaccessible replacement mappings to discard pages within the owned
address space; the current milestone is verified on Windows.

The original caller `824E0CB0` places base and size at stack offsets `+84/+80`,
calls import `82ACB98C` at `824E0DD4`, then uses the returned range in `824DF880`
and updates its own heap accounting. Audit artifacts are
`out/nt-decommit-next-audit.md` and `.json`. The observed request is base
`40070000`, size `10000`, mode `4000`, debug `0`.

Reproduction uses the unchanged generated source and verified local assets:

```powershell
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-coherency
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets
```

The same-run log `out/decommit-boot.log` records:

```text
RESULT NtFreeVirtualMemory status=0x0 base=0x40070000 size=0x10000 reclaimed=0x10000 reserved=0x260000
RESULT NtFreeVirtualMemory status=0x0 base=0x40030000 size=0x30000 reclaimed=0x30000 reserved=0x260000
ORIGINAL_TEXTURE_CREATE_RETURN result=0 output=0x701320b4 texture=0x4006ff00
ORIGINAL_TEXTURE_CREATE_RETURN result=0 output=0x701320bc texture=0x4006ff50
ORIGINAL_VIRTUAL_FREE_REQUEST base_ptr=0x70130ef0 size_ptr=0x70130ee0 type=0x8000 base=0x40260000 size=0x0
STOP virtual-memory-request @0x40260000: only exact MEM_DECOMMIT is supported
LAST_FUNCTION __savegprlr_26 @0x82a56060 LR=0x824e2134 calls=5728
```

The two decommits reclaim a total 256 KiB of commitment, retaining 0x260000 bytes
of virtual reservations at that point. Original 256x256 and 128x32 creation calls
now return zero after the earlier 64x64 and 128x128 calls. The fifth, 1024x1024
texture completes transfer/unlock and its shared-surface header heap free; its
creation call has not returned, because DDS source cleanup reaches a whole
reservation release request. That reservation at 40260000 has size 410000,
originally requested as 400030 and rounded by the game's 64 KiB virtual heap.
Supporting decommit alone cannot satisfy this distinct MEM_RELEASE operation.

All 38 native CTest and 152 Python tests pass with no skips. The new decommit
tests failed against unimplemented stubs, then passed with real host operations.
They cover native MEM_RESERVE/MEM_COMMIT state, zeroed discarded pages, live
neighbors, repeated and reserved-only operations, distinct adjacent private
allocations, private/placeholder mixtures, budget reuse, guard failure atomicity,
heap rounding, retained protection and output aliases. A split-allocation
destruction test passes with the existing destructor: a review concern was
disproved by that test and the documented
[VirtualQuery scan behavior](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualquery).
No speculative teardown change was made.

Logs: `out/decommit-memory-red.log`, `out/decommit-virtual-red.log`,
`out/decommit-green.log`, `out/decommit-full-build.log`, `out/decommit-ctest.log`,
`out/decommit-python.log`. `out/decommit-teardown-red.log` retains its original
investigation filename but **passed**, disproving the proposed teardown defect.
The local checkpoint JSON records executable SHA-256, source commit and command.
No original draw, Present, title or confirmation-to-menu transition has occurred.
