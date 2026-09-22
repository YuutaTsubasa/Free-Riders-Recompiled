# Checked original lhzu translation

Goal remains original Windows title and keyboard/controller confirmation into the
original main menu. This step removes the actual 824EC028 rejection without
replacing the game's vertex declaration allocator or builder.

Audit: 444 plain lhzu emissions, 441 unique addresses, all real opcode 41, all
RA nonzero and distinct from RT. Creator 824EC028 and builder 824EBF38 each have
only one missing lhzu. The original caller passes table 821A94D4 (POSITION,
COLOR, terminator); allocation and declaration metadata must remain original.

Runtime: compute modular full64 EA from RA plus sign-extended signed16 D. Read
checked big-endian halfword at low32(EA), then zero-extend into full64 RT and
update full64 RA. Failed read changes neither register. Reject aliased RT/RA
references defensively. No CR/XER changes; preserve current scalar unaligned
access policy. Pinned Xenia 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
InstrEmit_lhzu, agrees with this full64 computation and zero extension.

Generator: accept only exact logged empty plain lhzu blocks, canonical register
and signed16 decimal syntax, nonzero RA distinct from RT. Preserve all labels,
comments, line endings, original addresses and other rejection policies. Reject
mixed events, existing emission, malformed/unlogged/indexed/dotted forms. Resolve
only matching events in each independently inspected body; report counts and
addresses. Never modify upstream output or pins.

- [x] Runtime tests first: endian/zero extension, signed bounds, full64 carry and
      wrap, low32 addressing, odd address, alias rejection and failed reads.
- [x] Generator tests first: exact matching, invalid forms, mixed events, CRLF,
      labels, overlaps and union with existing rewrites.
- [x] Generate diagnostic-lhzu, audit retention delta and original word fields,
      build and boot original entry with actual assets; capture next stop.
- [x] Independent specification and quality reviews, relevant full regressions,
      evidence/docs and local commit. Keep title/menu goal incomplete.

Verified: four original declarations completed with exact metadata and element
bytes in the same run. Next native-device boundary824BBCE8, LR82498E40,
1623entries. All27native and132Python tests pass without skips. Independent
specification and quality reviews approve runtime, generator and read-only trace.
