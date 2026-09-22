# Original decoder partial vector loads

Continue the real Windows boot blocked at original decoder 824E46A0. The full
title-to-menu objective remains unchanged and unproven.

1. Verify the retained runtime test stubs fail. Implement checked left/right
   partial vector loads using the audited architecture slices and reversed host
   vector representation. Preflight only selected bytes; aligned right reads
   nothing. Preserve destination on failure and reservations on reads.
2. Recognize only exact pinned XenonRecomp emissions for lvlx/lvrx and their 128
   variants. Preserve uint32 effective-address calculation, reject altered
   emissions, and verify all left/right mask rows before publishing generated
   sources. Add positive and mutation regressions before implementation.
3. Connect diagnostic hooks, generate a new diagnostic directory, and verify
   all 266 original decoder instruction comments, six partial loads, four
   partial stores, scalar overlap loop and tail merges remain represented.
4. Build and run appropriate native/Python checks, then launch the real ROM
   from the original entry. Record the actual next stop or decoding evidence.
   Unit tests alone do not prove original decoding, presentation or menu input.
5. Review specification and code quality, fix findings, record evidence and
   source version. Do not push or replace original decoding with a host codec.

RED verified: out/partial-load-red.log reports the missing left-load stub.
Semantic evidence: out/partial-vector-load-semantics.md and
out/original-async-read-next-audit.json (private generated-source audit).

Implemented and reviewed. Native vector test GREEN; full verification passes
55 CTest and 192 Python tests. Whole decoder audit preserves all 266 instructions
and all other emissions apart from established hooks/checkpoints. Real boot
passes the old decoder stop and reaches XamNuiGetDeviceStatus at 82ACB43C.
Original decoded-output content, Draw/Present and title/menu remain unverified.
