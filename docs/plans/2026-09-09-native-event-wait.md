# Original worker event wait

Previous turn progressed to real original worker execution in5d0208d. GuestID3
now requests NtWaitForSingleObjectEx82ACB63C on event7210000C, mode1,
alertable0, relative timeout-80000ticks (8ms). The title/menu goal is unachieved.

Retain a duplicate Windows synchronization handle while the execution permit is
held. A prepared wait owns only that handle and copied timeout, never a reference
to guest memory or the mutable object registry. It remains valid if another
guest thread closes the original handle or its registry entry disappears.

Native wait uses WaitForMultipleObjects on a per-call manual cancellation event
and the duplicated target. A stop callback signals cancellation; cancellation
has the lowest wait-array index, preserving an already-signaled auto-reset event
when both are ready. Return actual signaled/timeout statuses, and represent
session cancellation separately from guest NTSTATUS. All owned handles close
after callback unregistration and completion.

Lease.run_blocking registers the blocked guest identity and releases execution
ownership, runs only native waiting code, then reacquires through the FIFO queue
before any guest state access. Blocked IDs remain reserved. A shared stop_source
unblocks native waits; request_stop runs outside the scheduler metadata mutex.
Shutdown drains active owners then joins blocked workers while dependencies live.
Preserve the first actual callback failure even during cancellation.

Guest adapter supports mode0/1, non-alertable waits, null infinite timeout and
checked non-positive signed64 relative timeout (100ns units). Round finite waits
up to host milliseconds; retain uint64 duration and split values exceeding the
largest finite DWORD wait into finite chunks. Positive absolute waits and
alertable/APC semantics remain explicit stops until required and implemented.
Invalid owned synchronization handles return INVALID_HANDLE. Other object
namespaces remain an explicit unsupported profile. No successful wait is invented.

TDD: actual signal/timeout, semaphore/event consumption, cancellation, duplicate
survival after close/registry teardown, blocking permit handoff/reacquisition,
failure propagation, blocked duplicate identities, stop-callback reentrancy,
timeout sign/bounds and invalid-profile preflight. Run original worker to its next
real dependency, independent spec/quality review, full regressions and local
commit. Keep title/menu goal active.
