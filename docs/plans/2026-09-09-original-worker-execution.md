# Execute original workers on native threads

Previous turn progressed to NtResumeThread82ACB54C in7f8b5c6. The original
request resumes the second owned parked worker72200008, previous-count output
70131450. Title/menu goal remains unachieved.

Use real native threads with serialized guest execution. A FIFO execution gate
grants one thread access to all guest memory/shared runtime at a time. Scheduler
metadata uses its own mutex so a newly resumed worker can enqueue while the
parent still executes. Parent publishes resume outputs and suspend state, waits
for worker admission, then yields. No worker bypasses its original XAPI entry.

Instrument every original branch label, function entry and import boundary with
safe points, including arithmetic-only loops. Handoff is deferred during known
reservation windows, but cancellation and budgets still apply. Preserve the
existing checked scalar/vector/atomic operations under exclusive ownership.
This is a conservative scheduling model, not parallel guest CPU execution.

Use thread-local trace/context identity with verified per-thread PCR/KTHREAD/TLS.
Share one TLS allocation bitmap across independently registered value banks;
free clears the slot in every bank before reallocation. Current registry and
memory metadata need no concurrent mutation once every guest access is gated.

Cancellation must use a distinct internal exception so diagnostic RuntimeStop
inspection catches cannot swallow it. Capture the first worker failure and its
trace before releasing the permit; notify the parent and preserve the cause.
Shutdown releases the main permit, stops scheduling, requests all workers stop,
waits for the active permit owner to drain, joins workers while their registry and dependencies remain alive, then destroys
assets, graphics, memory and the registry. Native cancel_and_join preserves the
owned handle until final destruction. Never join while holding the permit.

Unsupported blocking imports remain real stops until implemented with retained
native handle ownership, cancellation-aware waits and permit reacquisition.
Critical-section contention likewise cannot be replaced with a success return.

Tests: FIFO exclusivity and fairness using actual host threads, registration
handshake, cancellation even with handoff deferred, failure preservation,
waiting-worker shutdown, TLS bank independence/global slot reuse, resume output
preflight and real native resume count. Generator regression ensures labels get
safe points without altering original instruction effects or audit exclusions.
Build regenerated diagnostic, execute original entry to real worker activity
and its next dependency, review spec/quality, run relevant full regressions,
document reproducible results and commit locally. Full title/menu goal stays active.

Result: original NtResumeThread now starts guestID3 on its actual created host
thread, executing824DD0A8→824DCE98→824B1E58→824B2320. Worker-owned critical
sections enter/leave correctly. Next actual stop is NtWaitForSingleObjectEx
82ACB63C LR824D2868, event7210000C, mode1, alertable0, relative-80000ticks.
Main/worker total2039calls; clean diagnosticexit3 preserves worker failure.
32native/133Python tests passed; spec and quality reviews approved. Evidence:
docs/original-worker-execution.md. Original title/menu still not reached.
