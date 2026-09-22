# Original indexed primitive restart enable

The real74-shader entry reaches824E7F68/LR827F6414 with the known device andr4=1.
Original packet emission maps device+2948 to PA_SU_SC_MODE_CNTL, whose bit21
is multi_prim_ib_ena. The original setter consumes only r4's low bit, preserves
the remaining word (including cull), and always ORs the64-bit dirty word with40.

Retain this state as an optional native bool without inventing the reset index,
topology, index format/endian or a draw. Accept all uint32 inputs using the low
bit. Preflight both output ranges before any reads/effects and publish native
state last. Keep the void ABI and restrict the device hook to the audited
function/LR. Test full-device byte preservation, other state, repeated values,
high bits and late readonly/import/atomic failures. After RED/GREEN, run the
real original entry and record its next actual dependency.
