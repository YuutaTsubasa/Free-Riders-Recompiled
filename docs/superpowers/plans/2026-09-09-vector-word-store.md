# Vector word store implementation plan

> **For agentic workers:** Use superpowers:subagent-driven-development for the isolated native helper while the primary agent integrates generation; independent spec then quality review follows.

**Goal:** Execute verified stvewx/stvewx128 emissions with a checked four-byte store, independently of unresolved Etx services.

**Architecture:** Extend the existing byte-oriented vector memory helper, preserving the full-vector reversed register representation. The generator matches the exact pinned two-line emitter, retains the local `ea` assignment, and replaces only its store with a checked hook. All other vector forms remain unchanged/rejected.

**Tech stack:** C++20, Python unittest, Clang Windows diagnostic, existing CTest vector target.

## Contract and decision

Effective addresses wrap to 32 bits and align down to four bytes. Guest vector lane is `(EA & 15) / 4`, internal byte index is `12 - (EA & 12)`. Store exactly four bytes, reversing the chosen internal bytes to guest order; do not access other guest bytes or alter source registers. Normal GuestMemory guards, logical mappings, computed read-only words and active reservations apply. This follows the pinned emitter's `v.u32[3 - ((ea & 0xF) >> 2)]` selection while avoiding host-endian union access.

Chosen over merely accepting the existing macro: a byte-oriented helper can be tested independently of generated context types and host union layout. Full element loads and partial vector transfers have separate semantics and remain unsupported. No new Etx behavior or boot-progress claim is part of this checkpoint.

## Native component

Files: `src/vector_memory.h`, `src/vector_memory.cpp`, `tests/vector_memory_test.cpp`.

- [x] Add the API and behavioral tests, first with a no-op skeleton; observe a failed expected-byte assertion. API: `void store_vector_word(GuestMemory&, uint32_t, const VectorBytes&);`.
- [x] Verify all offsets 0..15 with distinct 16-byte register data and sentinel neighbors, a four-byte logical mapping (not 16), final address FFFFFFFF, missing final byte, guards, read-only provider without callback, reservation interference, and source preservation.
- [x] Implement alignment, lane extraction and `memory.store<uint32_t>()`; fresh isolated compile/run must pass:
  `C:/msys64/mingw64/bin/g++.exe -std=c++20 -Isrc tests/vector_memory_test.cpp src/vector_memory.cpp src/guest_memory.cpp -o out/vector_word_test.exe`.

## Generator and runtime

Files: `scripts/generate_diagnostic.py`, `tests/test_diagnostic_generation.py`, `src/diagnostic_main.cpp`, `src/diagnostic_hooks.h`.

- [x] Add tests for exact stvewx and stvewx128 emission, all register/address forms, CRLF, preserved `ea`, original inputs, report counts, missing/wrong masks and mismatched/no-op emission. Observe RED before implementation.
- [x] Match only the exact instruction plus `ea = (EA) & ~0x3;` and `PPC_STORE_U32(ea, ctx.vN.u32[3 - ((ea & 0xF) >> 2)]);`; use runtime `sfr::store_vector_word(ea, ctx.vN.u8);`. Register bounds are 0..31 / 0..127. Preserve the assignment and reject mismatched lane expressions, registers, widths or addresses.
- [x] Count `vector_word_stores` only for retained functions, include it in vector-instruction audit totals, and require the existing VectorMaskL validation for word-only rewrites too.
- [x] Add a runtime adapter that copies sixteen source bytes into VectorBytes and invokes the helper; no PPCContext flag/GPR mutation. Generate fresh `out/recomp/diagnostic-vector-words` and build using `scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-vector-words`.

## Verification and completion

- [x] Independent spec then quality review, resolve findings.
- [x] Full Python suite with all four native environment variables, CTest, pinned dependency verification and real boot; expect the same Etx stop at 347 unless evidence proves otherwise.
- [x] Record fresh source-site counts and exact evidence/limitations in vector/progress/toolchain docs and README, then commit only source/docs locally.
