# Original doubleword reservation operations

The Windows original title/menu goal remains **unachieved**. Checked `ldarx` and
`stdcx.` now let the original data-structure initialization pass `8274F4B8`.
The next real stop is graphics initialization at `824C1048`, LR `824994D4`, which
receives native device handle `71600000`. No original draw/Present or title/menu
input transition has occurred.

## Original instruction and emission evidence

`out/doubleword-original-audit.json` compares all 71 related instruction
annotations with 71 distinct original image words. Opcode, extended opcode,
register fields and record bit match. The first blocked function contains:

| Address | Original word | Instruction |
|---|---|---|
| 8274F7B4 | 7C80F8A8 | ldarx r4,0,r31 |
| 8274F7C0 | 7D40F9AD | stdcx. r10,0,r31 |
| 8274F7D0 | 7C80F9AD | stdcx. r4,0,r31 |

The instruction definitions are in IBM's [PowerPC v2.02 Book II, pages 24–25
(PDF pages 31–32)](https://powerpc.dev/general/PPC_Vers202_Book2_public.pdf).
The pinned XenonRecomp emitter is `ddd128bcca99fe8bfbb99bea583c972351fa6ace`,
`XenonRecomp/recompiler.cpp` cases `PPC_INST_LDARX` and `PPC_INST_STDCX`.

The source filter accepts only matching annotations and full canonical emitted
blocks, including address expressions, register widths and CR0 fields. Review
found that matching only a prefix could retain an appended assignment overriding
the checked result. New 32/64-bit regression cases first failed, then passed with
the complete-block boundary check. Altered or incomplete pairs remain stops.

## Runtime profile

One shared reservation stores width, address and the complete value. Loads and
conditional stores validate the complete aligned ordinary writable range before
changing reservation state or bytes. Doubleword access is big-endian and aligned
to eight bytes. Read-only/import fields and write-combined memory are rejected.
Successful loads replace the previous reservation. Matching conditional stores
consume it, compare the full value and write only on actual success; a valid
store without a reservation fails without altering memory. Unsupported width or
address mismatches stop explicitly. CR0 receives LT=GT=0, EQ=the actual result,
and SO=XER.SO only after checked access completes.

The guest execution permit prevents other guest threads from running inside a
live reservation. Ordinary checked writes and guest import calls in that window
remain guarded. This preserves the existing diagnostic profile; it does not
claim hardware reservation-granule, interrupt, DMA or concurrent raw-host-write
semantics. Existing module registration behavior is unchanged.

## Actual startup and source comparison

`out/doubleword-final-boot.log` records original CPU execution performing:

```text
STORE_CONDITIONAL_DOUBLEWORD address=0x400019a8 value=0x4002fe0000000001 success=1
STORE_CONDITIONAL_DOUBLEWORD address=0x40001980 value=0x4002fdf000000001 success=1
STORE_CONDITIONAL_DOUBLEWORD address=0x40001980 value=0x4002fde000000002 success=1
STORE_CONDITIONAL_DOUBLEWORD address=0x40001980 value=0x4002fdd000000003 success=1
```

The original code then creates three additional suspended native workers,
guest IDs 7–9, original worker `8249EF00`, arguments `83E544F4`, `83E544FC`,
`83E54504`, each with its requested `40000`-byte guest stack. Eight native workers
now exist; IDs 3–6 execute original XAPI/worker code while IDs 2 and 7–9 remain
suspended as requested by the game.

The recorded run exits 3 at:

```text
STOP native-graphics-function @0x824c1048: original function cannot consume an unimplemented native object layout
LAST_FUNCTION sub_824C1048 @0x824c1048 LR=0x824994d4 calls=2883
```

Function counts and timeout completion counts depend on native scheduling.
Original doubleword writes and the next stop are checked independently in the
actual-boot regression.

`out/recomp/diagnostic-doubleword` retains 31,388 functions and rejects 15,115:
11 more original functions now run through checked memory hooks. Retained sites
include 487 word reservation loads, 520 word conditional stores, 23 doubleword
loads and 46 doubleword stores. All input/log hashes are unchanged and there are
no newly rejected functions (`out/doubleword-generation-comparison.json`).

After tightening the canonical-block check, a fresh generation in
`out/recomp/diagnostic-doubleword-strict` produced byte-identical contents for
all 187 C++/header files, identical counts and rejections, and identical input
hashes (`out/doubleword-strict-equivalence.json`). The executable's generated
sources therefore also match the final strict generator.

Reproduce using the prepared local image/assets:

```powershell
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-doubleword
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets
```

Next inspect the actual device consumers in `824C1048`; preserve original CPU
initialization and connect required graphics operations to real native resources.

Verification: `out/doubleword-final-build.log`; 33/33 native CTest targets in
`out/doubleword-final-ctest.log`; 136 Python tests with no skips in
`out/doubleword-final-python.log`; pinned dependencies verified in
`out/doubleword-dependencies.log`. Specification and quality reviews approved
the final implementation. The module-registration regression found by the full
suite was removed before the final successful runs.
