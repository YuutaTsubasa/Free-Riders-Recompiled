# Native whole reservation release for original DDS cleanup

The Windows title/menu goal remains **unachieved**. After native decommit allowed
four original texture creations to return, the fifth 1024x1024 texture completed
transfer and unlock but stopped while freeing its original DDS source buffer.
This change provides the required whole virtual reservation release.

The original parser destructor 82537110 frees its owned pixels at 40260030.
Original heap code 824E1EE0 follows the large-allocation branch, subtracts its
headers to recover 40260000, unlinks its bookkeeping and releases its critical
section, then calls NtFreeVirtualMemory with type 8000, input size 0, debug 0.
The earlier allocation requested 400030 bytes and received a 410000-byte
reservation. All these values are hexadecimal. Original code converts the
returned NTSTATUS into its own heap-free Boolean; no shim bypasses that code.

The [pinned Xenia ABI](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc)
returns the unchanged base and **full released reservation size**, not zeroed
output values. The adapter supports exact MEM_RELEASE with zero input size and
debug 0. It requires the exact base of one owned virtual reservation. Interior or
unknown bases return C0000001 without output mutation; null base returns
C00000A0; addresses outside the supported virtual arenas return C000000D. Other
flags, unaligned bases, debug heaps and nonzero input sizes stop explicitly.

Both complete output words are checked before reading them. Neither may overlap
any part of the full released reservation, including words straddling its edges.
Service ownership is removed only after successful native release; the returned
size comes from that ownership record. A released gap reports protection 0 and
becomes reusable.

GuestMemory requires an exact logical reservation and validates its rounded host
span. It rejects active CPU reservations, import variables, computed read-only
words and unsupported write-combined backing before effects. Committed metadata
may span multiple guest reservations, so removing the released portion must keep
both neighbors. Every metadata removal and host operation is prepared first.

Windows restores each complete private backing allocation with its full size and
`MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER` operation, including allocations with
decommitted pages. Untouched placeholders remain reserved. This preserves the
continuous 4 GiB address-space envelope while discarding old contents, as
specified by [Microsoft VirtualFree](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualfree).
The size rule for this combined operation was verified on the native host:
`out/release-preserve-probe.log` shows zero size rejected with error 87, while the
full 65536-byte allocation size succeeds and restores MEM_RESERVE/PAGE_NOACCESS.
Both fully committed and partially decommitted allocations were checked. The
generic zero-size rule for bare MEM_RELEASE must not be applied to this combined
placeholder operation. The game-facing release input remains zero; the host call
uses the separately validated private backing size.
The backing ledger is then removed along with logical ownership, so a later
allocation can replace the placeholders and receive fresh zero pages. POSIX uses
an anonymous PROT_NONE replacement within the owned span. Partial native failure
is terminal; discarded bytes cannot be restored by a fictional rollback.

Primary audit and original instruction evidence: `out/nt-release-next-audit.md`
and `.json`. Reproduction uses the existing generated source and local assets:

```powershell
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-coherency
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets
```

The same-run log `out/release-boot.log` records:

```text
RESULT NtFreeVirtualMemory status=0x0 base=0x40260000 size=0x410000 reclaimed=0x410000 reserved=0x260000
ORIGINAL_TEXTURE_CREATE_RETURN result=0 output=0x701320c4 texture=0x4006ffa0
ORIGINAL_TEXTURE_CREATE_RETURN result=0 output=0x70132144 texture=0x4002ff40
NATIVE_GRAPHICS_BOUNDARY address=0x827f61a0 lr=0x82224440 r3=0x71600000 r4=0x83e53ca8 r5=0x4 r6=0x4 r7=0x0 r8=0x83e50000 r9=0x821980c4
STOP native-graphics-function @0x827f61a0: original function cannot consume an unimplemented native object layout
LAST_FUNCTION sub_827F61A0 @0x827f61a0 LR=0x82224440 calls=13354
```

There are now **21 original texture-creation returns with result 0**, including
the fifth source cleanup and all subsequent observed calls through output
70132144. Original heap trimming continues using real decommit; its existing
reserved ranges remain intact until explicitly released. The next stopped
function is renderer initialization receiving the native device handle; its
device state operations need further audit and native adaptation. No original
draw, Present, title, or confirmation-to-menu transition has occurred. Successful
CPU texture creation and transfer do not establish native GPU texture upload.

All 38 native CTest and 152 Python tests pass with no skips. New tests cover
exact logical release with host-rounded tails, remaining commitment accounting,
partially/fully decommitted and reserve-only allocations, separate private
chunks, unchanged neighbors in merged committed metadata, larger reuse across
old placeholder boundaries, zeroed reused contents, full-target output aliases,
provider/import/atomic/cache guards, and native reservation/envelope lifetime.
Combined-invalid decommit requests retain their earlier status precedence.

Logs: `out/release-red.log` (missing implementation), `out/release-green.log`
(native zero-size call rejected), `out/release-preserve-probe.log` (isolated API
evidence), `out/release-corrected-green.log`, `out/release-full-build.log`,
`out/release-final-build.log`, `out/release-ctest.log`, and
`out/release-python.log`. The probe source and binary are retained under `out/`.
The local checkpoint JSON records the source commit, executable SHA-256 and
same-run command. No generated source or pinned tool versions changed.
