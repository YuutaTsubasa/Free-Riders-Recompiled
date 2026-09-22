# EtxProducerRegister investigation

The context-only checkpoint reached this unresolved import after 347 function entries and stopped. The current native port instead applies the explicit unavailable-service policy below; a successful Xbox tracing implementation remains unresolved.

## Current native-port policy

Only the exact register (`0x82ACC28C`), unregister (`0x82ACC29C`) and log (`0x82ACC2BC`) import name/address pairs return zero-extended `0xC0000002` in r3 and log `UNAVAILABLE`. This is the `STATUS_NOT_IMPLEMENTED` constant in pinned Xenia `xbox.h` and Marathon `kernel/xdm.h`. Choosing it is a host policy, not evidence that Xbox Etx returns this status for these arguments. The helper cannot access guest memory or context; dispatch preserves other registers and reads or writes no producer pointer, handle or enable bits. The existing reservation guard applies first. Other unknown imports still stop.

The original observed caller ignores the unsuccessful result and continues without patches. The Etx-policy checkpoint reaches 523 function entries and stops at `XexLoadImage` (`0x82ACB71C`, LR `0x824D1E6C`), requesting `xam.xex` from the image string at `0x82000910`. This does not establish behavior for consumers, callbacks, successful registration or later Etx callers. At that checkpoint there were 14 bounded implemented imports, with these three unavailable policies counted separately.

Evidence: `out/etx-unavailable-boot-red.log`, `out/etx-unavailable-build.log`, `out/etx-unavailable-boot.log`, `out/etx-unavailable-tests-final.log` (94 tests, no skips) and `out/etx-unavailable-ctest-final.log` (11/11). No title screen or menu input is available yet.

## Observed call

The import is at `0x82ACC28C`, with LR `0x8290B240` and SP `0x7013FBD0`. The call instruction is at `0x8290B23C`, inside `sub_8290B170`. `LAST_FUNCTION sub_82A56710` is the last entered function, not this caller; that helper returned earlier. The new `IMPORT_CONTEXT` record distinguishes these facts.

| Register | Raw observed value | Low 32 bits / observation |
| --- | --- | --- |
| r3 | `0xFFFFFFFF820992A4` | `0x820992A4`, points into image data |
| r4 | `0xFFFFFFFF82AE8D60` | `0x82AE8D60`, points into a table |
| r5 | `0x4` | constant 4 |
| r6 | `0x0` | constant 0 |
| r7 | `0x40` | constant 64 |
| r8 | `0xFFFFFFFF82B76A30` | `0x82B76A30`, also passed to unregister |
| r9 | `0xFFFFFFFF820A0000` | scratch register used to construct r3 |
| r10 | `0xFFFFFFFF82AF0000` | scratch register used to construct r4 |

Only six arguments are explicitly prepared in this caller. Printing eight ABI registers does not establish an eight-argument signature. The caller immediately replaces r3 with its own object pointer after returning; it does not inspect a registration return value on this path.

The second statically found registration call is at `0x8276F760`, with return address `0x8276F764`. Its prepared low 32-bit values are r3 `0x82098FC4`, r4 `0x82AE8CC0`, r5 3, r6 0, r7 64 and r8 `0x82B5F398`. This call has not been executed in the current boot. Both r8 regions contain 64 zero bytes in the initial decoded image; that observation is not a runtime memory capture or a verified output-layout definition.

`sub_8290D058` passes `0x82B76A30` in r3 to `EtxProducerUnregister` at call address `0x8290D078`. Logging wrapper `sub_8290F4F8` loads the 64-bit word at its first argument plus 8 and tests bit 0 before calling `EtxProducerLog`; its disabled branch produces `0x40050002`. These observations suggest a producer context and enable bits, but do not establish all registration writes, status codes, callback behavior, table sizes, or valid failure contracts.

## Reference limits and next evidence

Xenia's [export table](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_table.inc) assigns registration ordinal `0x334`, unregister `0x335` and log `0x332`. The [Canary missing-kernel-function issue](https://github.com/xenia-canary/xenia-canary/issues/754) also lists registration as unresolved. Neither establishes a callable implementation contract. No Etx hook was found in the pinned Marathon/Unleashed reference implementations inspected locally. A separate fork's release notes advertise stubs, which are insufficient evidence for implementing this service.

Before implementing registration, establish the context layout and state transitions from a trustworthy public implementation or controlled observations, including register/unregister with consumers disabled and enabled, invalid inputs and the meaning of its return value. An explicitly designed host tracing replacement would need to define and test those effects against the game's accesses. Merely returning success, setting an invented handle, or assuming all-zero context is a successful registration would hide the current gap.

Evidence remains local and ignored: `out/import-context-boot.log`, original generated call sites in `out/recomp/ppc/ppc_recomp.140.cpp`, `.113.cpp` and `.141.cpp`, and the fingerprint-verified image. The context-only change passes 91 Python tests and 10 native tests; independent specification and quality reviews passed. This checkpoint adds diagnosis, not another supported import or a new boot milestone.

The subsequent [native module identity checkpoint](native-modules.md) acquired the XAM namespace identity and advanced to `XexGetProcedureAddress`, 524 function entries. It adds one bounded implemented import independently of the three Etx unavailable policies.

The latest [party-export policy checkpoint](party-export-policy.md) executes the original missing-service path and reaches 1,030 entries before an untranslated subfze blocks the next function.
