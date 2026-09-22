# Plain carry arithmetic in the original texture transfer

The Windows title/menu goal remains **unachieved**. Checkpoint `ae1e044`
completes the original DDS parsing/copy and reaches texture transfer `8250C848`.
Its two unsupported plain `addc` instructions are now translated with the
checked generator's canonical-source rules.

## Instruction semantics

The [pinned Xenia ALU emitter](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/cpu/ppc/ppc_emit_alu.cc)
adds full GPR values but derives carry by explicitly truncating both operands
to 32 bits. Its [GPR implementation](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/cpu/ppc/ppc_hir_builder.cc)
loads and stores 64-bit values. For this fixed Xbox profile:

```text
destination = (left64 + right64) modulo 2^64
CA = (uint64(left32) + uint64(right32)) > 0xFFFFFFFF
```

Incoming CA is ignored and overwritten, including noncanonical old values.
Plain `addc` does not change CR, overflow or sticky overflow. Source r0 is an
ordinary register. Value parameters preserve both operands when the destination
aliases either or both sources. This is not a claim about arbitrary PowerPC modes.

`FFFFFFFF + 1` produces `100000000` with CA1; `FFFFFFFF00000000 + 100000000`
produces zero with CA0. These distinguish full result width from carry width.
Tests also cover high bits, full wrap, source preservation, aliases, ignored old
carry, and the original subsequent self-`subfe` mask expression.

The actual original words are:

| PC | Word | Instruction | Carry consumer |
| --- | --- | --- | --- |
| `8250CA9C` | `7D4A2814` | `addc r10,r10,r5` | `subfe` at `8250CAA8` |
| `8250CAAC` | `7D6B2814` | `addc r11,r11,r5` | `subfe` at `8250CAB4` |

The resulting masks are consumed by original `and` instructions at `8250CAB0`
and `8250CABC`; no masks or transfer results are supplied by the runtime.
`out/next-addc-audit.json` checks all 46 plain annotations in 28 functions against
original machine words and all 46 matching log events. There are no variants
in this input. Primary-source details are retained in `out/next-addc-audit.md`.

## Strict generation and verification

Only exact plain `addc rD,rA,rB` comments with exclusively matching unsupported
events and empty instruction bodies are resolved. Canonical empty labels may
intervene, but inline statements and statements after labels reject the block.
Variants, invalid operands, existing implementations and conflicting logs remain
rejected. Instruction comments and labels/checkpoints remain unchanged, and
unrelated events continue to reject their containing function.

Fresh generation `out/recomp/diagnostic-addc` retains 18 additional functions,
with no newly rejected functions and unchanged input/log fingerprints. It has
31,655 retained functions and 14,848 rejected functions; 26 `addc` sites occur
in retained code. Other audited sites remain inside functions rejected for
other reasons. `out/addc-generation-comparison.json` records these differences
and validates the current generator hash. Counts alone are not game completion.

Arithmetic and generator tests failed before implementation and passed after it.
The generator suite contains 65 cases. Logs include `out/addc-runtime-red.log`,
`out/addc-runtime-green.log`, `out/addc-generator-red.log`,
`out/addc-generator-green.log`, and `out/addc-generator-suite.log`.

## Immediate addme dependency

The first rebuilt original-entry run (`out/addc-final-boot.log`) enters
`8250C848`, then stops at `824F29E8`, whose two plain `addme` instructions were
unimplemented. This actual source format `1A200054` has bit `100` clear: the
original branch skips `8250CA9C`/`8250CAAC` in this request. Removing the whole
function rejection permits original transfer entry, but does not establish
execution of those two `addc` sites.

Both `824F2C90` and `824F2CE0` are word `7D4A01D4`, `addme r10,r10`.
The same pinned Xenia implementation explicitly uses full GPR arithmetic with
minus one and incoming carry, and truncates operands for carry calculation:

```text
require oldCA in {0,1} before any mutation
destination = (source64 + oldCA - 1) modulo 2^64
CA = oldCA || (source32 != 0)
```

Value parameters support aliases; r0 is an ordinary source. Plain form leaves
CR/OV/SO unchanged. Unlike `addc`, noncanonical incoming carry is invalid because
`addme` consumes it. Tests distinguish full64 results from low32 carry and cover
invalid-state atomicity and the original subfic/sign-bit/addme/and chain. That
chain zeros nonpositive low32 inputs and retains the original full64 positive
input; the implementation retains general instruction semantics at all sites.

`out/next-addme-audit.json` verifies 51 annotations in 36 functions, representing
50 distinct machine sites and 51 matching events (`82260A20` appears twice).
All words have opcode31/XO234/OE0/Rc0/reserved-RB0 and matching operands, with no
variants or conflicting events. The generator uses the same strict plain-block
rules with two register operands and separate retained-site accounting.

Native RED/GREEN is recorded in `out/addme-runtime-red.log` and
`out/addme-runtime-green.log`; generator RED/GREEN is in
`out/addme-generator-red.log` and `out/addme-generator-green.log`. All 68 generator
tests pass (`out/addme-generator-suite.log`). Independent code review found no
actionable issues in the combined arithmetic and generation changes.

Fresh generation `out/recomp/diagnostic-addme` retains another 34 functions and
rejects no previously retained function. It retains 31,689 functions and rejects
14,814; retained code contains 42 `addc` and 49 `addme` sites. Additional `addc`
sites become reachable as generated code because their containing functions no
longer fail on `addme`; these counts do not establish runtime coverage. All
original input/log fingerprints are unchanged. The comparison and final
generator hash are in `out/addme-generation-comparison.json`.

## Actual execution and verification

```powershell
python scripts/generate_diagnostic.py --input out/recomp/ppc --log out/recomp/recompile.log --output out/recomp/diagnostic-addme
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-addme
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets
```

Generation requires a fresh output directory. The Windows native executable is
`out/build/host/sfr_cpu_diagnostic.exe`. `out/addme-final-boot.log` records this
same-run evidence from the verified original entry, with exit3:

```text
ORIGINAL_DDS_BLOCK_COPY_RETURN destination=0x40001ba0 source=0x84099f00 bytes=4096 swapped_pairs=1 source_unchanged=1 cache_zeroes=31
ORIGINAL_DDS_PARSER_STATE parser=0x70131090 pixels=0x40001ba0 format=0x1a200054 owned=1 width=64 height=64 depth=1
RESULT MmAllocatePhysicalMemoryEx base=0xf8afc000 committed_bytes=0x7204000 cache=write-combined
ORIGINAL_TEXTURE_TRANSFER_REQUEST r3=0x4002fea0 r4=0x0 r5=0x0 r6=0x40001ba0 r7=0x1a200054 r8=0x100 r9=0x70131140 r10=0x701310a8 sp=0x70130ff0 lr=0x8250e6e8
IMPORT KeGetCurrentProcessType result=0x1
STOP import-variable @0x82000664: __imp__VdGlobalDevice
LAST_FUNCTION __savegprlr_20 @0x82a56048 LR=0x824f1d8c calls=3290
```

The new observation reads actual register values only at `8250C848` with original
caller LR `8250E6E8`. The destination is a guest surface object, not the native
device handle; source points to the verified byte-swapped DDS payload. Original
layout calculation now proceeds past `824F29E8`. The next missing dependency is
the `VdGlobalDevice` variable. Entry counts can vary with worker scheduling.

The transfer has not been observed returning, so this is not evidence of
completed texture upload. Destination tiling must be established from actual
layout metadata before comparing physical backing; a 0x2000-byte allocation
does not establish a linear 4096-byte payload. `out/texture-transfer-flow-audit.md`
records the original argument provenance and a future exact return-observation
gate. No original draw, Present, title screen, confirmation input or main-menu
transition has occurred. The Windows title/menu goal remains **unachieved**.

All36 native CTest cases and148 Python tests pass with no skips, including the
68 generator cases and the real-entry regression. Logs are
`out/addme-final-build.log`, `out/addme-ctest.log`, `out/addme-python.log`, and
the RED/GREEN logs above. Independent generated-code review verifies both
`addme` calls and unchanged neighboring original carry/sign/mask operations;
all244 instruction comments in `824F29E8` remain in original order.
