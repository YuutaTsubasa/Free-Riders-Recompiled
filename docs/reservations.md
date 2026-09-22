# Checked single-thread word reservations

Historical word-only checkpoint. Current serialized 32/64-bit profile: [doubleword reservations](doubleword-reservations.md).

> Update (2026-09-22): with guest threads running in parallel, a conditional
> store that only compared values let lock-free lists succeed after an
> A-to-B-to-A change and link freed nodes, which surfaced as R6025 "pure
> virtual function call" aborts while Grand Prix loaded (`gp03`). Reserved
> addresses now hash to 1024 stripes, each with a lock and a count of its
> successful conditional stores; a reservation records the count and a
> conditional store succeeds only while it is unchanged (and the word still
> holds the reserved value). Unrelated words sharing a stripe can fail a store
> spuriously, which PowerPC permits. See `src/guest_memory.cpp`.

The diagnostic provides a bounded implementation of the `lwarx / stwcx.` pair. IBM's [lwarx reference](https://www.ibm.com/docs/en/aix/7.3.0?topic=set-lwarx-load-word-reserve-indexed-instruction) describes the indexed word load and reservation; [stwcx.](https://www.ibm.com/docs/en/aix/7.1.0?topic=set-stwcx-store-word-conditional-indexed-instruction) describes conditional storage, reservation clearing, alignment and CR0 effects. When RA is zero, the effective address uses RB alone; otherwise this diagnostic uses the32-bit sum. It validates the complete four-byte access.

One reservation belongs to the one supported guest thread. A successful checked load records address and value; a later successful reserve replaces it. Failed loads leave the preceding reservation unchanged. Conditional store with no reservation returnsfalse; a matching live reservation is consumed and the new big-endian word is stored. An unexpected changed backing value consumes the reservation and returnsfalse. Mismatched reserved/store addresses stop with `reservation-address`. Invalid alignment, mapped-range or permission checks stop before changing bytes or reservation state. Computed read-only fields are never eligible and their providers are not sampled.

Runtime adapters validate PCR, the current-thread pointer and thread ID. The load result is zero-extended into its destination GPR. Conditional-store completion clears CR0 LT/GT, places success in EQ and copies XER SO into CR0 SO, preserving XER and all other context. Faults stop before those flag assignments. The guest's MSR instructions, retry branch, arithmetic and function calls remain in generated code.

This is not a full reservation-granule or multi-thread implementation. An ordinary checked store, multi-write operation or import call during a live reservation stops with `reservation-interference` before its side effects. This deliberately bounds the supported window; it does not guess a Xenon cache-line size or emulate all reservation-loss events. Interrupts, scheduling, raw host-pointer writes and concurrent DMA are outside this model. The test-only backing edit exercises changed-value detection and is not evidence of concurrency or ABA correctness.

The generator recognizes only complete, exact upstream emission blocks: the instruction operands must match the emitted destination/source registers and address expression, including the complete original CR0/CAS/SO block for `stwcx.`. It replaces them with checked runtime hooks. Incomplete function-level pairs, unsupported widths, mismatched operands, or leftover reservation/CAS code receive `unsupported_reservation`; other raw memory remains rejected. Instruction comments and labels still describe original addresses, input files are preserved, and game functions are not replaced by success stubs.

The first observed affected function, `sub_82A52F10`, increments an initialization counter before initializing four objects when the new value is zero. It is reached from `sub_82ACA578`. This change executes the original control flow with checked reservation operations; it does not manually set the counter or skip initialization.

The regenerated `out/recomp/diagnostic-reservations` report contains46,503 functions and299 function imports. It retains29,928 functions and stops16,575:378 previously blocked functions now have checked memory operations. There are468 emitted reservation loads,498 conditional stores and27 monotonic time-base reads. Twelve rejected functions include `unsupported_reservation` among their reasons. These are source-site counts, not proof that all affected functions have run or that their remaining instructions are correct.

Native tests cover full32-bit data, exact endian bytes, reservation replacement/consumption, invalid ranges and alignment, read-only fields without callbacks, store interference, independent memory objects and the last aligned word of the address space. Generator regressions cover CRLF input, both address forms, operand/flag mismatches, unsupported widths and missing halves. Real startup provides separate coverage of the observed counter update and its next recorded stop.

Independent reviews found no correctness issues. Dedicated PPCContext tests for adapter failure paths and flag preservation are still absent; those paths were checked by inspection, while the real boot exercises successful conditional stores. This remains a validation limit.
