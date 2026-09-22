# Counted conditional branches in the original DDS parser

The original title/menu goal remains **unachieved**. Checkpoint `b97d922`
delivers the original `TEX_00` data to parser `8253A6A8`, which is blocked by
seven unsupported `bdzf` instructions.

## Instruction semantics

[PowerPC v2.02 Book I](https://powerpc.dev/general/PPC_Vers202_Book1_public.pdf),
Branch Conditional (printed p24 / PDF index33), decrements the full CTR before
testing it. In the current fixed 32-bit execution profile, the predicate uses
the resulting low 32 bits and the chosen CR bit. Existing `bdz`/`bdnz` output
and the [pinned Xenia branch emitter](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/cpu/ppc/ppc_emit_control.cc)
use this same width distinction.

The helper always performs modulo-64 decrement, then returns true only when
the low 32 bits are zero and the selected condition is false. It cannot change
CR, XER, LR or guest memory. In particular, CTR `100000001` becomes `100000000`
and may branch; CTR `100000000` becomes `FFFFFFFF` and does not branch. A true
condition suppresses branching but never suppresses decrement.

The original words all decode as opcode16, BO2, BI26 (CR6.EQ), AA0, LK0:

| PC | Word | Target |
| --- | --- | --- |
| `8253A768` | `405A0060` | `8253A7C8` |
| `8253A76C` | `405A00AC` | `8253A818` |
| `8253A770` | `405A006C` | `8253A7DC` |
| `8253A774` | `405A0040` | `8253A7B4` |
| `8253A778` | `405A0024` | `8253A79C` |
| `8253A77C` | `405A00B0` | `8253A82C` |
| `8253A780` | `405A0084` | `8253A804` |

The independent audit `out/bdzf-original-audit.json` checks all 1,271 original
annotations, 1,093 distinct addresses, against the actual decoded image words.
All use CR6.EQ; the rewrite also maps each explicitly named CR field/bit without
assuming EQ for other operands.

## Strict source transformation

Only canonical empty relative nonlink `bdzf` blocks with exclusively matching
unsupported-opcode log events are resolved. The target must be aligned, local,
within signed 16-bit branch displacement, and have an existing canonical label
at the exact instruction position. Labels cannot contain or precede leftover
implementation statements. No labels or destinations are invented. Unrelated
diagnostics still reject the containing function.

The branch helper is inserted before any following labels; existing label
checkpoints remain, including backward branches. Input files and unsupported
logs are never edited. A review found a misplaced-target-label gap, reproduced
by two failing tests, and it was fixed before building the final generated code.
Another review found that whitespace after a target colon bypassed the exact
checkpoint-label pattern. Space/tab fixtures failed before the fix; such labels
are now rejected, while canonical LF/CRLF labels remain accepted.

## Original-source audit and generation

The final output is `out/recomp/diagnostic-bdzf-final`. Compared with checkpoint
`b97d922`'s `diagnostic-doubleword`, it retains 245 additional functions and rejects
no previously retained functions: 31,633 retained and 14,870 rejected. Retained
functions contain 1,078 rewritten `bdzf` sites. Three functions with invalid
targets remain rejected, and all unrelated unsupported events remain effective.
These counts are coverage of checked generated code, not proof of a playable game.

`out/bdzf-generation-comparison.json` records the retention differences and
unchanged input/log fingerprints. `out/bdzf-final-equivalence.json` verifies
that all 187 generated C++/header files and all counts/input/log fingerprints
are identical after the additional strict-label fixes; the final report records
the current generator's SHA-256. The source image and raw recompiler output were
not changed.

## Actual original entry and next dependency

```powershell
python scripts/generate_diagnostic.py --input out/recomp/ppc --log out/recomp/recompile.log --output out/recomp/diagnostic-bdzf-final
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-bdzf-final
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets
```

Generation requires a fresh output directory. The executable is
`out/build/host/sfr_cpu_diagnostic.exe`; ROM/image/assets remain local and ignored.
The actual run in `out/bdzf-final-boot.log` still exits 3, now at a later import:

```text
ORIGINAL_DDS_PARSER_REQUEST parser=0x70131090 data=0x84099e80 size=0x1080 info_output=0x0 flags=0x1 lr=0x8250e25c
ORIGINAL_DDS_COPY_REQUEST parser=0x70131090 format=0x1a200054 source=0x84099f00 destination=0x40001ba0 row_pitch=0x100 slice_pitch=0x1000 width=64 height=64 depth=1 type=3 next_mip=0 next_face=0
IMPORT_CONTEXT name=__imp__MmQueryAddressProtect address=0x82acb6dc lr=0x825f87c4 sp=0x70130ce0 r3=0x40001ba0 r4=0x84099f00 r5=0x20001 r6=0x2 r7=0x800 r8=0x10 r9=0x40 r10=0x2
STOP import-function @0x82acb6dc: __imp__MmQueryAddressProtect
LAST_FUNCTION sub_824DF3D8 @0x824df3d8 LR=0x825f87c4 calls=3158
```

The original DDS handler `8253A190` recognizes DXT5 in the actual `TEX_00`
header and produces the observed 64×64×1 metadata. An observation at entry
`825F8B58`, exclusively with LR `8253A980`, reads the current image node in r30.
It cannot alter guest registers or supply parser results. Reaching this call
proves the format handler succeeded and the outer parser entered its requested
allocation/copy path. The outer parser has **not** returned successfully: copying
stops when original code queries destination memory protection. No texture
upload, draw, Present, title or menu transition has occurred. Function-entry
totals vary with native worker scheduling.

## Verification

35/35 native CTest cases and 139 Python tests passed with no skips. Tests cover
64-bit counter decrement, low-32 predicates, all 32 named CR bits, the original
seven-way dispatch pattern, malformed/unsupported source rejection, preserved
checkpoints and unresolved events, and the real boot/resource/worker/shader
path through the new stop. All three pinned dependencies verified unchanged.

Logs include `out/bdzf-runtime-red.log`, `out/bdzf-runtime-green.log`,
`out/bdzf-generator-red.log`, `out/bdzf-target-gap-red.log`,
`out/bdzf-label-newline-red.log`, `out/bdzf-label-newline-green.log`,
`out/bdzf-final-build.log`, `out/bdzf-observer-build.log`, `out/bdzf-ctest.log`,
`out/bdzf-python.log`, and `out/bdzf-pins.log`. Independent semantic/specification
and code-quality reviews approved the implementation after the label fixes.
