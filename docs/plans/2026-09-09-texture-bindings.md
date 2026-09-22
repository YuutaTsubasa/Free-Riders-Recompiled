# Original texture initialization loop

The verified6fb15e9 entry reaches824F4220/LR827F5FC8, slot0, texture0,
dirty-mask80000000. The original caller reads eight writable globals at82B62914
at execution time. Its full loop and setter are audited in
out/texture-bind-next-audit.md/json.

Preserve a native per-slot nullable identity. For the verified null/old-null
case, clear only fetch dword0 type bits0..1 and store the null binding pointer;
do not mark original texture dirty bits or change the other five fetch words.
Require the exact eight-slot mask and known device/caller, and preflight all
effects. Non-null new resources require actual native views/samplers/uploads;
non-null old resources require real fence retirement. Until implemented, reject
either before publication rather than guessing resource ownership or success.

Unset native slots remain unknown; after the original null call they are
explicitly unbound. Future draw preparation must translate that state into the
shader ABI's typed null descriptors; no descriptor or draw is fabricated here.
Validate exact guest-byte effects, independent slots, invalid profiles and
guards, then run all eight calls from the real entry and follow the next
resource/state dependency. In parallel, prepare a compiled-DXIL verification
candidate for the already-identified alpha comparison issue.
