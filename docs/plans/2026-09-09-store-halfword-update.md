# Checked original sthu translation

The preceding goal turn made progress: ade9ca0 established all68 original native
shader creation requests. Current actual stop is effect825E47D8, with60 missing
sthu emissions; first825E4954 is realwordB5090002, `sthu r8,2(r9)`. No other
rejection reason applies to that body. Complete log contains450 plain-sthu events
at447 unique addresses; an earlier substring count463 included13 sthux events.

Design: recognize exact logged empty plain-sthu emission blocks, with canonical
registers (RA nonzero) and signed16 displacement. Keep all original instruction
comments, control flow and other stop policies. Mixed diagnostics, malformed or
unlogged forms, existing emission and indexed variants remain rejected. Resolve
only matching events in each independently audited body; report retained counts
and addresses. Do not patch generated output manually or alter upstream pins.

The runtime helper accepts GuestMemory, a reference to the full64-bit base GPR,
the captured full64-bit source value and signed16 displacement. Compute modular
64-bit EA, store source low16 bits big-endian through checked GuestMemory at
low32(EA), then update base to the full64-bit EA only after store succeeds.
Source/base aliasing stores the pre-update value; no CR/XER changes. Existing
GuestMemory access and write-combine rules remain effective. This diagnostic does
not introduce a hardware alignment interrupt model.

Sources: NXP EREF2.0 section2.2 (p2-1) and sthu(p5-286),
https://community.nxp.com/pwmxy87654/attachments/pwmxy87654/mpc5xxx/9953/1/EREFRM_2-0.pdf
describe full64 EA calculation and memory addressing in32-bit mode; pinnedXenia
95a5c3ee250f80c3b9d139658649d9ffb6db3eec ppc_emit_memory.cc CalculateEA_i,
StoreEA and InstrEmit_sthu agree. Local reference: out/sthu-xenia-ppc-emit-memory.cc.
IBM update-form rules reject RA0 for PowerPC. The older upstream STWU/STBU
emission assigns only u32; it is not used as a semantic model for the new helper.

Tasks:
- [x] Generator TDD: exact log/emission matching, mixed events, malformed and
      zeroRA forms, displacement limits, aliasing, CRLF, labels and overlaps.
- [x] Runtime TDD: endian/truncation, source/base aliasing, signed offsets,
      full64 update and modular arithmetic, low32 memory address, unmapped and
      read-only/partial-bound failures preserving data and base.
- [x] Generate new diagnostic directory from untouched original output; audit
      real463 words and report of newly retained bodies. Build using the current
      prepared realshadercache and boot the original game, record next stop.
- [x] Independent spec and quality review; relevant full regression, docs and
      local commit. Keep original title/menu goal active and incomplete.


Verified result: originaleffectinitialization completes; nextstop824EC028/lhzu,
LR824B7DA4,1514calls. All26native and127Python tests pass. Bothcomponents approved
in independent spec and quality review. Evidence docs/store-halfword-update.md.
