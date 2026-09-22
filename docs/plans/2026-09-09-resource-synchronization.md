# Original resource synchronization toward texture transfer

The verified5701d32 boot reaches original824F1C08 and stops reading VdGlobalDevice.
This routine consumes original device ownership/ringbuffer layout and emits GPU
commands. The native device creation boundary did not initialize that layout;
merely publishing its opaque token cannot implement synchronization.

First capture actual resource-lock arguments and original caller/stack options.
Audit the complete routine, resource metadata, original caller and packet effects
against pinned primary sources. Identify a native operation that preserves the
original CPU resource/layout/transfer flow while enforcing actual queue completion
and coherent ownership of guest physical backing. Do not skip necessary GPU work
or invent original ringbuffer state. Unknown modes remain explicit failures.

Implement only once the observed operation and effects have an evidence-backed
contract; write distinguishing native/runtime tests first, verify RED/GREEN, then
run the genuine game entry to observe subsequent transfer work. Preserve existing
DDS byte oracle and establish original transfer return before claiming completion.
Full regression, independent review and local commit follow. The user title/menu
goal remains unachieved until original rendering and input transition are verified.

The captured request is824F1C08/LR824F3560: null token, operation14,
resource400007C0, base/rangeF8AFC000/2000, flags0 and mask02000000. Original
header100003 becomes100103 before the exact55-instruction coherence block.
That block issues TC cache coherence and waits for the busy bit to clear; it is
not an upload. Its registers mutated internally are dead before subsequent use.

Use a whole-function hash guard and verified branch edges to replace only
[824F1D88,824F1E64) with a native callback; retain original instruction comments
as provenance, CPU instructions outside the block, and the exit checkpoint.
The callback validates the original mode/header and one wholly owned writable
physical range, explicitly rejecting allocations marked GPU-backed. Queue
completion uses a real D3D12 fence with cancellation and bounded failure.
Keep the guest permit through validation/wait to prevent ownership races.
No native texture mirror currently exists for this allocation. Future GPU
publication must mark its allocation first and consume latest CPU bytes; this
CPU-only path must not be reused to reconcile native texture caches.
