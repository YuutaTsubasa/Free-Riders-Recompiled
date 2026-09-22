# Protection queries in the original DDS copy path

The Windows original title/menu goal remains **unachieved**. Checkpoint
`3ebc65d` reaches original DDS copy processing, where `825F8750` queries both
the newly allocated destination and the original image source. The exact
`MmQueryAddressProtect` import at `82ACB6DC` now returns their recorded guest
protection values; it does not supply a copy result or parser return value.

## Source semantics and ownership

The [pinned kernel query](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc#L413-L424)
returns protection bits directly. Its
[heap metadata](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/memory.cc#L1320-L1331)
retains the requested protection even for reserved-only pages. The current
virtual allocator supports only read/write (`4`), including reserve-only
regions whose bytes remain inaccessible. Physical allocations preserve their
read/write (`4`) or write-combined read/write (`404`) value.

Queries use immutable allocator-owned extents. Unallocated pages within their
supported arenas return zero, including unaligned queries into such pages.
Foreign mappings and unsupported heaps/aliases stop explicitly. Physical
allocation metadata is prepared before mapping and published only after mapping
succeeds. Querying cannot allocate, commit, read payload bytes or mutate them.

Image addresses use `ImageProtection`, constructed from the already verified
original XEX header. The
[pinned image loader](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/cpu/xex_module.cc#L982-L999)
uses security page descriptors: code and read-only data return `2`; data returns
`4`. Heap `[80000000,90000000)` selects 64 KiB pages. PE section names and the
diagnostic process's host read/write mapping do not determine these results.

The parser validates XEX magic, header size, optional-table boundary, security
offset/extent, expected image base/size, supported heap/page alignment, nonzero
descriptor counts/types, and exact full image coverage with 64-bit arithmetic.
It retains immutable query ranges. This adds query metadata; it does not add
general image write-protection enforcement or a protection-changing API.

The actual header SHA-1 is `82ef42d3f130b26563fcd85909f2eab613de6c68`.
Security offset `98`, size `3274`, and 522 descriptors cover the complete
`82000000..840A0000` image. Descriptor 521 at header offset `32F4` is word
`00000013`: one read-only page `[84090000,840A0000)`, containing the original
source `84099F00`. Its query result is therefore `2`. The complete byte-derived
audit is `out/protection-image-audit.json`.

## Actual execution

```powershell
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-bdzf-final
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets
```

The source generation remains that of the
[checked counted-branch checkpoint](counted-conditional-branch.md). The new
executable is `out/build/host/sfr_cpu_diagnostic.exe`. Actual
`out/protection-final-boot.log` records exit 3:

```text
ORIGINAL_DDS_COPY_REQUEST parser=0x70131090 format=0x1a200054 source=0x84099f00 destination=0x40001ba0 row_pitch=0x100 slice_pitch=0x1000 width=64 height=64 depth=1 type=3 next_mip=0 next_face=0
RESULT MmQueryAddressProtect address=0x40001ba0 protection=0x4 lr=0x825f87c4
RESULT MmQueryAddressProtect address=0x84099f00 protection=0x2 lr=0x825f87e4
STOP unsupported-function @0x825f8230: sub_825F8230: unchecked_guest_memory
LAST_FUNCTION sub_824DF3D8 @0x824df3d8 LR=0x825f88a0 calls=3152
```

The original copy-selection logic executed both queries and reached its next
copy routine. That routine still contains unchecked generated memory access and
remains rejected. The copy and outer parser have **not** completed; no texture
upload, draw, Present, title screen or main-menu transition is claimed.
Function-entry totals vary with native worker scheduling.

## Verification

36/36 native CTest cases and 139 Python tests passed with no skips. Three new
query test groups failed against initial throwing implementations, then passed.
Review caught unaligned free-address queries using an allocation API that
requires alignment; two additional regression cases failed before that fix
and passed after it. Tests cover source metadata bounds/types/coverage and
immutability, reserved-only access guards, page ends/free holes/foreign mappings,
read/write versus write-combine, unchanged allocation usage and payload bytes,
and the original boot through both queries. Existing dependency pins remained
unchanged (verified in `out/bdzf-pins.log`).

Logs: `out/protection-red-build.log`, `out/protection-red.log`,
`out/protection-unaligned-red.log`, `out/protection-final-build.log`,
`out/protection-final-focused.log`, `out/protection-ctest.log`,
`out/protection-python.log`.
