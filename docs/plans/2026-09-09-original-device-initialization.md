# Continue original device initialization

Previous turn made progress: 1d9e059 executes original lhzu and verifies all four
original vertex declarations. Current boot stops at824BBCE8/LR82498E40/calls1623.
The title/menu goal remains unchanged and incomplete.

The complete original824BBCE8 body only captures incoming device r3 in r30 and
stores it to83E53700. It never dereferences that device or passes it to a child.
Its one indirect call uses incoming r4's allocator vtable slot16, size derived
from incoming r5 and alignment128. Retain that call and all original code.

- Admit only this audited body with the established native device value.
- Build and run original entry with actual assets, then audit the next real
  boundary before changing it. Never invent successful allocation or GPU work.
- Update boot evidence/test to the actual result; independent review, relevant
  regressions and local source commit. Keep original title/menu goal active.

Actual continuation now reaches NtCreateSemaphore82ACB5BC at1901entries,
LR824D09E0: out701313A0, attrs0, initial1, maximum1. Implement a real Windows
unnamed semaphore owner, not a signaled flag or fabricated handle. Native layer
owns CreateSemaphoreW/WaitForSingleObject/ReleaseSemaphore/CloseHandle objects;
guest layer checks big-endian output memory before creating/publishing a handle.
Non-null object attributes remain an explicit unsupported profile until needed.
Invalid counts return invalid-parameter and clear output. Close and failure
paths release real handles. Use nonreused aligned guest IDs72100004..721FFFFC;
bound the existing file IDs below72100000 to keep registries disjoint.

Native tests must demonstrate real initial count, consumption, timeout, release,
previous count, over-release preserving count, close/stale IDs and cleanup.
Guest tests cover BE output, checked invalid/protected/partial output before any
native creation, unsupported attributes, invalid counts clearing output and
actual ownership. Wire only observed import semantics; add waiting/release guest
translation when the real flow requests it and its exact ABI is verified.

After real semaphore creation, original flow requests an auto-reset nonsignaled
event atNtCreateEvent82ACB55C. Generalize the typed native owner toNativeSyncObjects
and guest adapter toGuestSyncObjects, preserving semaphore behavior. Add actual
CreateEventW/SetEvent/ResetEvent primitives, mixed-kind ownership, type checks,
auto/manual-reset tests, and checked guest create-event import. No guessed
previous-state result or guest wait/release implementation is introduced.

Completed and verified: actual semaphore72100004, event72100008 and NtClearEvent
success; next ExCreateThread at1944entries, LR824D2518. All29native and132Python
tests pass, all3pins verify, independent spec and quality review approved.

Read-only next-thread audit: flags1=X_CREATE_SUSPENDED, worker must remain parked.
Actual request outputs70131360/FFCFD260, stack1000, startup824DD0A8,
worker824B1E58, contextFFCFD240. Pinned Xenia clamps stack to at least4000 and
aligns4K. Parent824B1D28 stores handle atcontext+48 and ID atcontext+32, then
references the thread object, sets priority/affinity and later resumes it.
Startup824DD0A8 invokes thread callbacks824DCE98(1), worker, callbacks(0), then
ExTerminateThread. A direct call to the worker would skip original startup.
Worker dispatches its argument vtable+36; actual live target needs capture.

Before worker resume, provide per-thread PPCContext, stack, PCR/KTHREAD and
static/dynamicTLS, generalize main-thread-only context guards and reservation
state, implement actual contended synchronization and protect mutable registries.
Native suspended creation may progress before resume, but requires owned
shutdown/join and propagation of worker stops. Do not fabricate a thread handle,
worker completion, object layout, priority/affinity result or resume success.
