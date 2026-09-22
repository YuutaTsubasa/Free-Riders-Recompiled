# Genuine original suspended worker creation

Previous turn made progress:6a0ff1e creates native semaphore/event objects and
clears the event. Actual ExCreateThread82ACB77C request: output70131360,
stack1000, thread-ID outputFFCFD260, XAPI startup824DD0A8, worker824B1E58,
contextFFCFD240, flags1. The original title/menu goal remains unachieved.

Implement the observed suspended creation profile using a real Windows native
thread created with CREATE_SUSPENDED and its own prepared guest state. Preserve
the XAPI entry and register arguments; never skip directly to the worker.
Guest resume remains unsupported until per-thread runtime context, shared memory,
critical sections, reservations and registries are ready for concurrent execution.

NativeThread owns one real thread and cooperative stop source. Callback receives
stop_token; normal resume uses actual ResumeThread and returns its previous
suspend count. Native exception capture is rethrown on join. Shutdown requests
stop, resumes a never-started native thread only to cancel before the guest
callback, joins, and closes the handle. No TerminateThread or detached workers.
Expose native ID and entry-started observation for evidence. The host stack is
separate from the guest stack. Tests prove initial suspension, actual foreign
host thread execution after resume, exit status/exception propagation and owned
cleanup of parked/cooperative running workers.

GuestThreads validates both outputs and startup targets before allocating state.
Use disjoint checked guest slots73000000+i*200000 (32 slots initially), with
PCR at+0, KTHREAD at+1000, TLS at+2000, lower stack guard+20000, stack
limit+21000, upper guard after stack. Stack is max(4000,align4K(request)) with
default40000 from actual XEX optional header20200; cap1MiB per slot explicitly.
Reserve the whole slot then commit only the documented structures/TLS/stack.
Each thread gets independent static TLS copied from the original XEX template
and zero dynamic values, guest ID2 onward, opaque handles72200004 onward.
Current main-thread TLS allocator and context guards are not reused as worker
state. Known PCR/KTHREAD fields follow pinned Xenia; unsupported process pointer
access is guarded. No thread-object reference or priority/affinity is fabricated.

Prepare a distinct PPCContext through a factory: r1=stack_base-100, r13=PCR,
r3=worker,r4=context and original startup mapped function. Initialize host FP
state on the actual worker, not the parent. The production guest resume import
is not connected yet; startup is still parked throughout this checkpoint.

Validate memory/namespace collisions, output bounds/overlap, TLS metadata and
flags before native creation or output publication. Host/factory failure stops;
owned native creation rolls back on publication failure. Reserved guest slots
are session allocations, not exposed as successful threads after a failed create.
Record actual native ID, distinct guest state and suspended/no-entry evidence.
Run original entry to next actual dependency, review independently, regressions,
docs and local commit. Keep the full title/menu goal active.

Result: implemented and verified real suspended creation. The original call
published handle72200004, guestID2, PCR73000000 and a real Windows native ID;
entry_started remained0. Observed original virtual worker target824B2320.
The next actual stop is ExThreadObjectType820007E8 in824CFE68, LR824B1DB0,
calls1945. All31native and132Python tests passed, including actual-ROM boot.
Full evidence and remaining concurrency constraints: docs/native-thread-creation.md.
