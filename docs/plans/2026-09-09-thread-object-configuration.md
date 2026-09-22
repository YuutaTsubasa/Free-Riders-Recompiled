# Original thread object configuration

Previous goal turn made progress:0781926 creates the actual first suspended
Windows thread; original execution now stops on ExThreadObjectType820007E8.
Title/menu acceptance remains unachieved.

Continue the original priority/affinity wrappers rather than bypassing them.
Bind the thread-type export to an owned opaque identity72300000. Reserve its
address without readable descriptor storage: callers may pass its identity to
the adapter, but unsupported field access still stops. Reference-by-handle
resolves the existing real thread to its prepared KTHREAD, checks the type and
output, and accounts for explicit references. Dereference balances that count.
Closing the guest handle invalidates future handle lookups but does not cancel
a live thread. Keep its native execution ownership until session teardown or a
future supported exit; explicit references and handle lifetime are separate.

Use real Windows priority and affinity operations on the owned native thread.
Map guest priority increments through pinned Xenia's documented implementation:
above34=>2, above17=>1, below-34=>-2, below-17=>-1, otherwise0. Return the actual
previous host relative priority as that reference does; do not claim exact Xbox
scheduling equivalence. For observed single-bit guest affinity0..5, select the
corresponding allowed host process CPU, wrapping only on hosts with fewer than
six available processors. Never change the process affinity mask. Store guest
CPU identity and previous guest affinity only after output preflight and host
success. Unsupported multiple-bit masks stop explicitly.

NativeThread implements checked Get/SetThreadPriority, GetProcessAffinityMask,
SetThreadAffinityMask and GetThreadGroupAffinity. Unit tests verify actual host
results and unchanged suspension. GuestThreads tests cover identities, types,
balanced references, closed handles retaining live threads, checked outputs,
priority mapping and affinity/PCR/KTHREAD updates. Run the original entry to
the next real dependency; guest NtResumeThread stays unsupported until shared
runtime concurrency is ready. Review spec and code, run regressions, document
the actual next stop and commit locally. Keep the full goal active.

Implemented: two actual original parked threads now complete balanced object
references and actual Windows priority/affinity calls. Next original request
is NtResumeThread82ACB54C(handle72200008,previous_count70131450), LR824D011C,
calls2004. All31native and132Python tests passed with the real-ROM boot enabled.
See docs/thread-object-configuration.md for same-run evidence and remaining work.
