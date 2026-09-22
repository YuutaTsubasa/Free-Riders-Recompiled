# Original named synchronization objects

Continue the actual NtCreateEvent request for Event_NuiGetSkeleton, notification
event initially nonsignaled. Preserve original creation, ownership publication,
thread startup and waits; do not signal it merely to advance the game.

Parse the original 12-byte big-endian attributes: root FFFFFFFC, ANSI_STRING
pointer, flags80. Validate the aligned writable handle output and snapshot the
complete attributes/8-byte ANSI descriptor/exact Length bytes before kernel
effects. Support this flat root/flags form with nonempty printable ASCII names;
unknown roots, flags, nested paths or encoding stop explicitly. No terminator
or MaximumLength-sized read. Overlapping output is safe after input snapshot.

Extend the native registry with a process-local guest object namespace shared
between event and semaphore kinds. Create real unnamed Windows objects. Same-kind
exact-name reopen retains the same guest handle, status40000000 and shared current state,
following the pinned Xbox kernel model. A case-fold-equal but differently spelled
name stops explicitly until the Xbox case policy is verified;
never reinitialize an existing object. Different-kind collision stops explicitly
until its Xbox status is pinned. Preserve unnamed semantics, cancellation and
retained native wait duplicates. Remove a name after the last guest handle closes,
without invalidating an already retained wait. Multiple guest opens retain the
name until all opens are closed. Keep ID allocation and host object
publication exception safe and verify no host handle leak.

Native owner and guest parser receive independent tests; first prove RED, then
implement and run GREEN. Independently review namespace, ABI and lifecycle against
original bytes and primary reference sources. Boot the real ROM with the explicit
ntsc-us runtime profile and record actual named creations and subsequent stop.
Run complete regression tests, record executable/source/command/log evidence and
commit locally. Title/menu completion still requires actual rendering and input.
