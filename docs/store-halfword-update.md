# Original effect initialization: store halfword with update

At checkpointade9ca0 the true game entry created all68 basic shader resources and
executed the original device AddRef, then stopped before effect function825E47D8.
Its only rejection reason was60 unimplemented plain `sthu` instructions. The
first instruction at825E4954 has real image wordB5090002, `sthu r8,2(r9)`.
The complete log contains450 plain-sthu events at447 distinct original addresses.
All447 words have primary opcode45 and nonzero RA. Some displacements are odd.
The audit is `out/sthu-real-word-audit.tsv`; indexed `sthux` remains separate.

## Semantics and boundary

The helper computes a modular64-bit sum of the original base GPR and signed16
displacement. It writes the original source low16 bits in big-endian order at the
low32 effective address, then replaces the full base GPR only after successful
storage. Capturing the source by value preserves an aliased RS/RA. It never
modifies CR or XER. These operations follow [NXP EREF2.0 section2.2 and
sthu p5-286](https://community.nxp.com/pwmxy87654/attachments/pwmxy87654/mpc5xxx/9953/1/EREFRM_2-0.pdf).

The [pinned Xenia memory emitter](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/cpu/ppc/ppc_emit_memory.cc)
independently uses full64 `CalculateEA_i`, a16-bit byte-swapped store, and `StoreEA`
without truncating the updated register. This is reference source only; the
runtime here executes compiled original C++ and checked native memory helpers.

PowerPC update-form RA0 is invalid according to the [IBM update-form
rules](https://www.ibm.com/docs/en/aix/7.3.0?topic=processor-fixed-point-load-store-update-instructions)
and EREF; it is rejected during generation. A valid nonzero-numbered base GPR
may itself contain zero. RS may equal RA for this store instruction.

GuestMemory preflights both bytes, enforces reservations and read-only/import
guards, and completes write-combined stores before the base update. Existing
unaligned scalar access behavior is retained. This does not implement hardware
alignment interrupts; an address-space-crossing access explicitly stops rather
than partially writing or pretending to emulate a wrapped multi-byte access.

## Generation policy

Only exact canonical empty-emission `sthu rS,D(rA)` blocks with matching
unrecognized-sthu log evidence are lowered. Missing/mixed log evidence, malformed
registers or displacement, indexed/dotted forms and existing partial emission
remain rejected. Original comments, addresses, labels and control flow remain.
Each overlapping function is inspected independently. Other missing instructions
continue to reject their containing function. The upstream tool checkout and
original generated input are unchanged.

## Runtime verification

`out/sthu-runtime-red-test.log` demonstrates the missing implementation;
`out/sthu-runtime-green-test.log` passes the real helper tests. These exercise
big-endian truncation, aliasing, signed limits, full64 carry/wrap, low32 memory
addressing, zero displacement, unaligned storage and failure-before-base-update
for unmapped, partial, protected and address-space-crossing stores. Existing
GuestMemory tests separately cover import/reservation/write-combined behavior.
Independent semantics review approved the helper against the cited sources.

The title/menu goal still requires original execution through effects, graphics
state/bindings, actual draw/Present and confirmation input. A translated
instruction or successful helper test is not completion of that goal.


## Real generation and original-entry result

```powershell
python scripts/generate_diagnostic.py --input out/recomp/ppc --log out/recomp/recompile.log --output out/recomp/diagnostic-sthu
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-sthu
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets
```

Generation refuses to replace an existing destination. The retained realshader
cache is the prepared cache from ade9ca0; rebuild it with the commands in
[native-shaders.md](native-shaders.md) when preparing a new checkout.

`out/sthu-generation.log` reports31,198 retained and15,305 rejected functions,
including197 checkedsthu emissions and the previous72subfze. The target effect
function contains60 checked stores. Compared with diagnostic-subfze,84 functions
are newly retained, none newly rejected, and all input-file hashes are unchanged.
`out/sthu-retention-audit.json` and `out/sthu-emission-word-audit.log` record these
independent comparisons.

`out/sthu-native-boot.log` proves the original68 shader creations remain, and
section82AD440C is entered and normally left after original effect initialization.
The next stop is824EC028 (`lhzu` at824EC050), LR824B7DA4, after1514entries.
LAST_FUNCTION82484088 is the previous entered function; the stopping library
body is not executed. Its next dependency concerns original vertex declaration
creation; no function or successful result was fabricated to pass it.

Final verification: `out/sthu-final-build.log`, `out/sthu-final-ctest.log`26/26,
`out/sthu-final-tests.log`127Python/no skips, and `out/sthu-final-dependencies.log`
all3pins. Runtime and generator received independent specification/semantics and
quality approval. Original title/menu and input acceptance remain unachieved.

Independent next-boundary audit identifies824EC028 as CreateVertexDeclaration:
the actual original caller824B7DA0 passes static table821A94D4. Verified image
records describe stream0/offset0/FLOAT3(002A23B9)/POSITION0 and stream0/offset12/
D3DCOLOR(00182886)/COLOR0, followed by the stream255 terminator. Both this creator
and builder824EBF38 are rejected solely for lhzu (824EC050 and824EBF7C). They
allocate ordinary guest heap storage, initialize the real declaration metadata
and copy elements; no GPU work is submitted. Checked lhzu lowering is therefore
the next concrete dependency. Later native binding can decode those genuine
guest objects; this CPU allocation should not be replaced with a success stub.
