# Original asynchronous read completion

Continue the real NtReadFile at LR824DD208: file72000008, event72100034,
APC0, context/IO82B50114, buffer401A3834, length20000, explicit offset pointer.
The full original title-to-menu goal remains active.

Implement and validate genuine native OVERLAPPED requests, private completion
events and aligned staging. Native immediate/pending/EOF/cancellation results
must determine guest results. Protect pending guest data/IO ranges from writes
and mapping changes, preserving normal checked stores and write-combine barriers.
Use retained event ownership and the same FIFO execution scheduler for host
completion publication; no invented PPC thread or direct OS writes to guest RAM.
After complete data and BE IO publication, signal only the supplied read event.
Original worker10 remains responsible for header parsing and its separate
completion notification to guest3.

Use independently implemented native I/O and completion primitives while the
root owns guest memory and the coordinator. Build meaningful RED regressions
before GREEN implementations, then integrate and run the original entry. Cancel
and drain all native requests and join completion workers before destruction.
Do not present a request, pending status or successful unit test as a completed
original read or title/menu milestone.

Implemented and verified: actual OVERLAPPED pending read delivered 14774 FNT_SE
bytes with the original asset SHA1, followed by the real read-event wake, original
header-completion signal, caller wake and original XCTD recognition/decode path.
55 native and 188 Python tests pass. Reviews caught and fixed failed native result
state, pending error drain, shutdown while submission remained in flight, and
admission allocations preceding guest effects. Next real stop is original CPU
decoder824E46A0's unchecked partial vector loads; decoder completion and the full
title/menu goal remain unproven. See ../native-async-read.md and ignored checkpoint.
