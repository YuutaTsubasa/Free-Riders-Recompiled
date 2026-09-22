# Original XEX embedded resource lookup

6da84b4 reaches XexGetModuleSection(71000040, TEX_00, 70131754, 70131750).
Original header key 000002FF at offset330C has length356 and22 entries. Each
entry is eight name bytes, BE address, BE size. TEX_00 is84099E80/1080.
Pinned UserModule::GetSection and utf8::equal_z compare exact names through
their first NUL, accept a full eight-byte name, and preserve outputs on absence.

Parse immutable resource metadata from the validated supplied header before
publishing module mappings. Reject malformed table lengths, overflowing ranges
and duplicate normalized names. Do not create resources or substitute content.
Use the existing loader identity only. Unknown handles return C0000008; absent
names return C0000225. Invalid guest pointers remain explicit checked stops.
Read a bounded guest name before output writes; resource addresses/sizes must
refer to completely mapped data before publication. Preflight both nonnull,
aligned, distinct writable output words so a bad second word cannot partially
publish the first. Names may alias outputs after capture. Preserve metadata
against guest header mutation; no allocations during queries.

TDD resource presence/absence/case/eight-byte names, immutable metadata, exact
content and output ordering, bad-second-output preservation, malformed tables
and invalid ranges. Bind exact import82ACB72C, observe real boot and original
resource fingerprint, run native/Python regressions, independent spec/quality
review, then local commit. Title/menu remains unachieved.

Actual resource lookup reaches the original texture wrapper8250E960. Independent
instruction audit confirms E960 and E188 only check/forward the opaque device
before the original DDS parser8253A6A8. Add only these exact device+address+LR
exceptions: E960/8243D1AC, save82A56058/8250E968, E188/8250E9C4,
save82A56030/8250E190. The two stack helpers do not dereference the device.
Verify real original DDS pointer/size reaches parser, retaining the explicit
unsupported parser stop. No texture creation result or device layout is supplied.
