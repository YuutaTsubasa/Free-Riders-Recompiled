# Explicit unavailable Etx policy

**Goal:** Let the original Windows game initialization handle a truthful service-unavailable result, without inventing a successful tracing registration or output layout.

This is a native-port policy, not an implementation or verified emulation of Xbox Etx. The observed registration call ignores its return register. The game image initially contains zeroed producer contexts and observed logging wrappers check enable bits. We will return the existing NTSTATUS `STATUS_NOT_IMPLEMENTED` value for Etx producer register, unregister and log. No guest pointer is read or written by the policy. All other missing imports still stop. No successful producer handle, callback, consumer or event delivery is claimed.

## Bounded implementation

- [x] Add a small pure policy helper that matches exact known import name/address pairs (register 0x82ACC28C, unregister 0x82ACC29C, log 0x82ACC2BC), returns optional status C0000002, and rejects every other pair. Write matching/mismatching tests and observe behavioral RED before implementing.
- [x] Use the helper after the existing active-reservation interference check. Assign only zero-extended r3 status, emit `UNAVAILABLE` with status and return. Do not change memory, other registers or import support counts. Add build/test target and a real boot regression requiring an explicit unsuccessful Etx result; observe RED on the preceding binary.
- [x] Build and execute the real entry, recording whether the unmodified caller handles the failure and what actually stops next. If the caller depends on a successful service or errors out, retain that evidence; do not skip its branch or patch game data to force progression.
- [x] Independent spec then quality review; run full Windows Python/native regression and pinned dependency verification. Update Etx investigation and progress to distinguish policy from implementation, source-site counts from executed behavior, and diagnostics from goal completion.

The chosen non-success return replaces the diagnostic's unconditional exception only for these three explicitly optional tracing operations. This policy does not generalize to graphics, input, memory, file loading or unknown imports. It can be replaced by a real tracing implementation later if the game requires one.
