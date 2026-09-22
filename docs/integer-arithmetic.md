# Bounded integer arithmetic hooks

## Plain SUBFZE

The genuine entry previously stopped before `sub_82211798` because `subfze r4,r8` at `0x82211D68` had only an instruction comment and no emitted implementation. Two logged occurrences at that address come from overlapping generated definitions.

Reading that location in the fingerprint-verified decoded image yields `0x7C880190`: primary opcode31, RT4, RA8, XO200, OE0 and Rc0. This independently confirms the exact plain instruction at the boot blocker. It is a check of this location, not blanket validation of every generated instruction comment.

Plain SUBFZE computes ones-complement source plus the old Carry flag, and updates Carry while leaving CR and overflow flags unchanged. The [IBM instruction definition](https://www.ibm.com/docs/en/aix/7.1.0?topic=set-subfze-sfze-subtract-from-zero-extended-instruction) distinguishes plain, record and overflow-enabled forms. This checkpoint supports only the plain form.

The diagnostic follows the Xbox carry model in pinned Xenia's [subfzex emitter and AddWithCarryDidCarry helper](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/cpu/ppc/ppc_emit_alu.cc#L520): full 64-bit GPR result with carry calculated from low 32-bit operands. The native hook uses unsigned arithmetic for defined wraparound:

- Result: `~source64 + old_carry` modulo 2^64.
- New Carry: `old_carry == 1 && uint32_t(source64) == 0`.
- Only destination and Carry references may change; source is passed by value, allowing the same source/destination register.
- Carry values outside 0/1 stop before mutation. This is not support for arbitrary MSR modes or OE/record variants.

The tests cover zero, all ones, signed boundaries, nonzero upper halves with zero lower halves, aliases and unchanged neighboring storage. In particular, `source=0x100000000, carry=1` yields `0xFFFFFFFF00000000` and Carry 1, distinguishing the selected low-32 carry rule from full-64 overflow.

## Generation boundary

Source rewriting must recognize a plain two-register SUBFZE comment with no emitted statements, correlate its computed guest address with only matching `unrecognized subfze` log events, and emit the native hook. Other event kinds/opcodes at that address remain a reason to stop. Each overlapping definition is inspected independently. Existing range, label, missing-symbol, memory and other unsupported-instruction checks remain in force; the raw log and upstream source are not changed.

Retained source-site counts are not execution counts. Enabling a function does not prove every instruction in it has run, nor does an arithmetic test establish the game's overall correctness or the title/menu outcome.

## Current native result

The generated directory is now `out/recomp/diagnostic-subfze`. It retains 31,114 functions (40 newly retained, none newly rejected) and rejects 15,389. Its 72 retained SUBFZE sites are static counts. Other retained hooks include 33 time-base reads, 472 reservation loads, 502 conditional stores, 7,244 full vector loads, 7,348 full vector stores and 145 vector word stores.

The real entry advances from 1,030 to 1,037 function entries and stops at a required memory query:

```text
IMPORT_CONTEXT name=__imp__MmQueryStatistics address=0x82acc44c lr=0x82a70c9c sp=0x701314c0 r3=0x70131510 r4=0x40000 r5=0x31 r6=0x6 r7=0x2 r8=0x100 r9=0x30 r10=0x4
STOP import-function @0x82acc44c: __imp__MmQueryStatistics
LAST_FUNCTION sub_82A70C78 @0x82a70c78 LR=0x82a70c9c calls=1037
```

Exit code is 3. The query occurs early in the newly enabled function, before its SUBFZE site; this boot proves the function can start, not that SUBFZE has executed. Original wrapper `sub_82A70C78` supplies a 104-byte statistics structure on the stack. The next implementation must define native memory accounting; pinned Xenia's statistics handler explicitly labels several values guessed, so copying those constants is not verification of the native allocator's state.

Final verification: 37 focused generator tests, 103 full Python tests without skips, 13/13 CTest targets and pinned dependencies passed. Independent spec review found a malformed-name rejection gap; new failing tests preceded the fix and both spec and quality reviews approved the final result.

The final generator was rerun to `out/recomp/diagnostic-subfze-verified` after that rejection-only fix. All 187 generated code/support files match the already-built directory byte for byte; reports differ only in generator SHA-256. The built generation used `f24f5a36d6a3db1ee44ac925bf7e3643cd06fd358af8b4c95d21906b2775d506`; the verified current generator is `fbf8f315cb9bd2ed15dae2db709fabe3c1ba8bb13014e4cbc9e1918c3fb93985`. This comparison is saved in `out/subfze-generation-comparison.log`; original generated artifacts remain intact.

Other ignored evidence includes `out/subfze-helper-red.log`, `out/subfze-helper-green.log`, `out/subfze-generator-red.log`, `out/subfze-generator-p2-red.log`, `out/subfze-generator-green.log`, `out/subfze-boot-red.log`, `out/subfze-build.log`, `out/subfze-boot.log`, `out/subfze-tests-final.log` and `out/subfze-ctest-final.log`.
