# Original lhzu and vertex declaration initialization

The preceding checkpoint 4cd2f84 completes the original effect initialization
after all 68 shader creation requests. Its next stop is original declaration
creator 824EC028 at 1514 function entries, LR824B7DA4. Both that creator and its
builder 824EBF38 are rejected solely for missing `lhzu` emissions (824EC050 and
824EBF7C). This change executes those CPU functions instead of substituting a
native declaration handle or success result.

## Instruction contract and sources

`load_halfword_update` computes full64 modular EA from RA plus signed16 D,
reads a big-endian halfword through checked GuestMemory at low32(EA), then writes
the zero-extended result to full64 RT and full64 EA to RA. No register changes
precede the successful read; CR/XER are untouched. The generator rejects RA0 and
RA==RT; the helper also rejects aliased destination/base references. Unaligned
access retains the existing scalar policy. This does not implement hardware
alignment interrupts or silently allow cross-address-space halfwords.

Pinned Xenia [ppc_emit_memory.cc](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/cpu/ppc/ppc_emit_memory.cc),
`InstrEmit_lhzu` and `StoreEA`, confirms full64 arithmetic and zero extension.
Local source copy: `out/sthu-xenia-ppc-emit-memory.cc`.
IBM [PowerPC invalid update forms](https://www.ibm.com/docs/en/aix/7.3.0?topic=assembler-detection-error-conditions)
specifies the nonzero and distinct RA restrictions. IBM's older 32-bit lhzu
description is not used to truncate Xenon's full64 GPR updates.

The source filter accepts only exact canonical plain-lhzu comments, matching
unrecognized-lhzu log events and empty emission blocks. It preserves original
comments, labels and line endings and inspects overlapping bodies independently.
Malformed, indexed, dotted, nonempty, unlogged and mixed-diagnostic blocks remain
rejected. Resolving lhzu does not remove other whole-function hazards.

Runtime tests check endian order and zero extension, signed16 extrema, carry
above bit31 and modular64 wrap, low32 memory address, odd address, computed
read-only values and alias rejection. Unmapped, partial, address-space-tail,
reserved, protected import and throwing-provider failures preserve both full64
registers. RED assertion is in `out/lhzu-runtime-red-test.log`; final CTest output
records the passing exit status. Generator tests exercise accepted/rejected
syntax, logs, existing emission, labels, CRLF, overlaps and coexistence with the
previous subfze/sthu rewrites.

## Reproduction

```powershell
python scripts/generate_diagnostic.py --input out/recomp/ppc --log out/recomp/recompile.log --output out/recomp/diagnostic-lhzu
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-lhzu
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets
```

Generation requires a fresh output directory. Use the existing prepared real
shader cache or reproduce it using [native-shaders.md](native-shaders.md).
ROM, assets, generated C++ and output evidence remain local ignored files.
The [original title/menu goal](title-menu-goal.md) remains incomplete until an
actual game draw/presentation and original confirmation transition are verified.

## Verified original-entry result

Generation retained 31,377 bodies and rejected 15,126; 179 newly retained, none
newly rejected, all original input hashes unchanged. It retains 343 checked lhzu
emissions. Previously implemented sthu/subfze counts rise to 308/74 because some
newly retained bodies contain those instructions too. All 444 logged lhzu words
and original comments agree; current image-loader bytes were also checked.
Evidence: `out/lhzu-generation.log`, `out/lhzu-retention-audit.json`,
`out/lhzu-real-word-audit.tsv`, `out/lhzu-emission-word-audit.log`.

The actual run in `out/lhzu-native-boot.log` reaches four completed original guest
declarations, observed read-only at entry824BBCE8 after the original caller's
stores. No declaration creation hook or guest write is used by the trace:

| Global pointer slot | Actual pointer | Original table | Elements |
| --- | --- | --- | ---: |
| 83E50A90 | 4002F930 | 821A94D4 | 2 |
| 83E50A94 | 4002F990 | 821AA938 | 3 |
| 83E50A98 | 4002FA00 | 821AA968 | 3 |
| 83E50A9C | 4002FA70 | 821A94F8 | 4 |

For every object, checked BE32 fields match header00100005, refcount1,
offset20=FFFF0000, count at24, maxstream0 at28 and offset48=0. The sixteen stream
mask bytes at32 are FF followed by zeros. All 12*count bytes at52 match original
input table bytes; the terminator is not copied. Every trace reports
`metadata_valid=1 elements_match=1`. The boot regression requires all four.

The next actual stop remains guarded native-device entry824BBCE8, LR82498E40,
1623 function entries. A read-only audit shows that its incoming device is only
saved to global83E53700. Its separate allocator callback uses incoming r4's
vtable slot16 and, in the observed call, requests size0/alignment128. The next
step is to retain this audited original device-saving function while following
that actual allocator path. No device guard exception is added in this change.

The title, menu, input acceptance and original draw/Present are still absent.

Final verification: `out/lhzu-final-build.log`, `out/lhzu-final-ctest.log`
(27/27), `out/lhzu-final-tests.log` (132 Python, no skips), and
`out/lhzu-final-dependencies.log` (all three pins). Independent specification and
quality reviews approved the runtime, source filter and read-only declaration
trace. The old boot test expected the preceding stop; its assertion was updated
to require all four actual declaration observations and the new guarded stop.
