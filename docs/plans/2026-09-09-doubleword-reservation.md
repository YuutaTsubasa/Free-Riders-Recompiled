# Checked doubleword reservation translation

18e0bd4 passes original worker event waiting and reaches 8274F4B8, whose generated
ldarx/stdcx. sequences still contain unchecked raw uint64 access and CAS.
Existing checked lwarx/stwcx. support is not sufficient for this original path.

Extend the same serialized-guest reservation profile to aligned big-endian
64-bit load-reserve/store-conditional. One shared reservation tracks address,
width and full value. Validation errors leave memory/reservation unchanged;
matching stores consume reservation and set actual success/failure. Unsupported
mixed width/address, ordinary writes and import/handoff interference remain
explicitly guarded. No host interleaving is allowed inside a live reservation.

Strict generator rewriting must match both the exact ldarx/stdcx. annotation
and pinned upstream emission before replacing raw accesses with checked hooks.
Retain labels/checkpoints; verify original instruction words and register fields.
Unknown/malformed, unpaired or mixed-only forms must remain rejected. Hook CR0
sets LT=GT=0, EQ=actual conditional result and SO=XER.SO only after checked access.

TDD runtime width/value/validation/consumption and generator exactness/rejection.
Generate a new diagnostic directory without editing inputs, compare reports and
hashes, build, then run actual startup to the next missing dependency. Full
regressions plus independent specification/quality review precede local commit.
The Windows original title/menu goal remains active and unachieved.
