# Checked cache-block zero in the original DDS copy

The original Windows title/menu goal remains **unachieved**. Checkpoint
`3d8579d` reaches original copy routine `825F8230`, which is rejected for the raw
`memset` generated from plain `dcbz` at `825F83CC`. Its original word is
`7C005FEC`, with RA zero, RB11, XO1014 and the plain-operation L field zero.

## Semantics and checked runtime

The [pinned Xenia emitter](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/cpu/ppc/ppc_emit_memory.cc#L1108-L1129)
distinguishes plain `dcbz` from its 128-byte variant. Plain `dcbz` zeros exactly
32 bytes aligned down to a 32-byte boundary. RA zero contributes literal zero;
the effective address wraps to the guest's 32-bit address space before alignment.

`GuestMemory::zero_cache_block` checks the complete aligned 32-byte range with
the existing write preflight, including logical bounds, import guards, computed
read-only words and live reservations. Only then does it clear actual bytes and
invoke existing store completion handling, including the write-combined fence.
Failed preflight cannot partially clear the block or consume a reservation.

Tests cover every alignment offset, neighboring bytes, partial logical mappings,
guards at the end of the block, provider non-sampling, reservation preservation,
the final address-space block, and Windows write-combined backing. Initial
throwing implementation failures are recorded in `out/dcbz-runtime-red.log`.

## Strict source transformation

Only an exact canonical plain instruction comment followed by its complete
matching 32-byte `memset` is replaced. Operand, size, mask, value or emission
changes, missing statements, and extra statements are rejected. Existing labels
and instruction comments remain for address auditing and checkpoint insertion.
No unsupported log events are resolved by this rewrite. The diagnostic hook
calls the checked memory operation and reports the first four successful calls.

`out/dcbz-original-audit.json` independently checks all five plain sites in four
functions against original image words. The 83 `dcbzl` sites in 39 functions are
audited separately and remain rejected; their distinguishing bit is `00200000`.

Fresh source `out/recomp/diagnostic-dcbz` retains four additional functions and
rejects no previously retained functions. It contains all five checked plain
sites, with 31,637 retained and 14,866 rejected functions overall. Input and log
fingerprints are unchanged; `out/dcbz-generation-comparison.json` records the
comparison and checks the current generator hash. These counts describe checked
generated-code coverage, not game completion.

## Original copy-return observation

The observation captures actual r3/r4 pointers and r5/r6/r7 mode, stride and count
on entry to `825F8230`, only for LR `825F88A0`. For mode `20001`, stride `2` and
count `800`, it snapshots 4096 source bytes. The original scalar conversion uses
`lhz` followed by `sthbrx`; the vector path derives the same permutation.

After the original copy returns, caller `825F8750` restores its 160-byte frame
and enters `82A560A4` with the same LR. That exact address/LR/SP combination
triggers comparison against the captured source with each adjacent byte pair
reversed, and a separate source-unchanged check. Observation state is thread-local
and cannot change guest registers, bytes or return values. Copy completion does
not by itself prove full DDS-parser completion, texture upload or rendering.

## Original texture parameter path

After the verified copy returns, the original outer parser updates its pixel
pointer/ownership and returns to `8250E188`. That caller reaches `8250D6E0` only
after a successful parser result. All nonnegative returns in the original parser
produce zero; this establishes success through original control flow rather
than a captured return-register value.

Three exact device-valued entries are now admitted:

| Original function | Required LR | Device usage |
| --- | --- | --- |
| `8250D6E0` | `8250E55C` | Null comparison and forwarding only |
| `8250C7E8` | `8250D768` | Replaces r3 with 1 before its sole callee |
| `82504FA8` | `8250D858` | Replaces r3 with output pointer before static-table copy |

Each requires the established device `71600000`. None dereferences its layout.
The existing `82A56044` stack-save allowance already covers its save helper.
`C7E8`'s callee `829337A8` is one original `blr`; `4FA8` copies the original
304-byte capability table at `82004B88`. No substitute capability data or return
values are supplied. Full original ranges and byte hashes are in
`out/texture-requirements-original-audit.json`.

A read-only observation at the first entry uses the caller's original parser
object at SP+160. Actual pixel pointer `40001BA0` and ownership `1` corroborate
the original post-copy update. The initial boot assertion failed at `D6E0`
before these audited allowances (`out/texture-parameters-red.log`).

## Actual execution and validation

```powershell
python scripts/generate_diagnostic.py --input out/recomp/ppc --log out/recomp/recompile.log --output out/recomp/diagnostic-dcbz
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-dcbz
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets
```

Generation requires a fresh output directory. The final executable is
`out/build/host/sfr_cpu_diagnostic.exe`; `out/dcbz-final-boot.log` records exit 3:

```text
ORIGINAL_DDS_BLOCK_COPY destination=0x40001ba0 source=0x84099f00 mode=0x20001 stride=0x2 count=0x800 sp=0x70130ce0
CACHE_BLOCK_ZERO effective=0x40001c00 aligned=0x40001c00 bytes=32
ORIGINAL_DDS_BLOCK_COPY_RETURN destination=0x40001ba0 source=0x84099f00 bytes=4096 swapped_pairs=1 source_unchanged=1 cache_zeroes=31
ORIGINAL_DDS_PARSER_STATE parser=0x70131090 pixels=0x40001ba0 format=0x1a200054 owned=1 width=64 height=64 depth=1
REQUEST MmAllocatePhysicalMemoryEx flags=0x0 size=0x2000 protect=0x404 min=0x0 max=0xffffffff alignment=0x1000
RESULT MmAllocatePhysicalMemoryEx base=0xf8afc000 committed_bytes=0x7204000 cache=write-combined
STOP unsupported-function @0x8250c848: sub_8250C848: logged_unsupported_instruction
LAST_FUNCTION __restgprlr_29 @0x82a560bc LR=0x8250e6e8 calls=3251
```

The original allocation/texture-transfer path obtained write-combined backing,
then stopped at two unsupported `addc` instructions in `8250C848`, at `8250CA9C`
and `8250CAAC`. Native GPU texture upload is not established. No original draw,
Present, title screen or main-menu transition has occurred. Entry totals can vary
with worker scheduling; copy comparisons and parser state are the substantive
evidence.

36/36 native CTest cases and 142 Python tests passed with no skips. The generator
suite contains 62 cases. Runtime and generator tests failed before implementation
and passed after it; source, specification and quality reviews found no remaining
actionable issues. Logs: `out/dcbz-runtime-red-build.log`,
`out/dcbz-runtime-red.log`, `out/dcbz-runtime-green.log`,
`out/dcbz-generator-red.log`, `out/dcbz-generator-green.log`,
`out/dcbz-generator-suite.log`, `out/dcbz-final-build.log`,
`out/texture-parameters-build.log`, `out/dcbz-ctest.log`, `out/dcbz-python.log`.
