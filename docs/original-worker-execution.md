# Original XAPI and game worker execution

Historical checkpoint at `5d0208d`. The current event-wait implementation and next CPU stop are documented in [native event waiting](native-event-wait.md).

The original title/menu acceptance goal is **not achieved**. A real Windows
worker now executes the original XAPI startup and original game worker. The next
dependency is an original event wait; no game draw, Present or title/menu input
transition is established.

## Reproduce the native run

```powershell
python scripts/generate_diagnostic.py --input out/recomp/ppc --log out/recomp/recompile.log --output out/recomp/diagnostic-workers
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-workers
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets
```

Generation requires a new output directory; reuse the prepared directory when
building again. The verified private ROM/image/assets remain local and ignored.
The expected current diagnostic exit code is3 on the unsupported wait.

`out/worker-runtime-boot.log` contains this evidence from one actual execution:

```text
RESULT NtResumeThread handle=0x72200008 output=0x70131450 status=0x0 previous=1 guest_id=3 backend=windows-thread
ORIGINAL_WORKER_BEGIN guest_id=3 host_thread=22512 startup=0x824dd0a8 worker=0x824b1e58 argument=0xffcfd1b0 pcr=0x73200000
WORKER_ENTER guest_id=3 name=sub_824DD0A8 address=0x824dd0a8
WORKER_ENTER guest_id=3 name=sub_824DCE98 address=0x824dce98
WORKER_ENTER guest_id=3 name=sub_824B1E58 address=0x824b1e58
WORKER_ENTER guest_id=3 name=sub_824B2320 address=0x824b2320
ORIGINAL_WAIT_REQUEST guest_id=3 handle=0x7210000c mode=0x1 alertable=0x0 timeout_ptr=0x73224c20 timeout_bits=0xfffffffffffec780
STOP worker-import-function @0x82acb63c: __imp__NtWaitForSingleObjectEx [guest_id=3 function=sub_824D2978 address=0x824d2978 LR=0x824d2868]
LAST_FUNCTION sub_8270D980 @0x8270d980 LR=0x824a34d0 calls=2039
```

The omitted creation record has native_id22512, exactly matching the executing
worker's host ID. IDs vary per run; the boot regression compares them within the
same run and checks original XAPI-to-worker function ordering. Both the original
startup critical section82AD08F0 and worker critical sectionFFCFD214 are entered
and left with worker object73201000. The main thread continues its own original
code between worker scheduling points.

The wait is for actual auto-reset event7210000C, user mode1, non-alertable0,
with a signed relative timeout of -80000 ticks (8 ms in100ns units). The native
wait adapter is not yet connected. First created worker72200004 remains parked;
the original code resumes its second worker72200008 first.

## Scheduling and state integrity

`GuestExecution` grants exclusive access to guest memory and runtime state to
one actual native thread at a time. FIFO admission and targeted readiness
handshake prevent the parent from proceeding before a resumed worker is queued.
The original code yields at function/import boundaries and every retained branch
label, including arithmetic-only loops. This is serialized guest execution on
native threads, not parallel guest CPU scheduling.

All148136 retained labels have exactly one checkpoint. The original input hashes,
function counts and rejection audit are unchanged:31377 retained functions,
15126 rejected functions,46503 total functions and299 imports. The generator adds
scheduling boundaries without replacing original instruction behavior. Evidence:
`out/worker-generation-audit.log` and the generated `report.json`.

Known reservation windows retain exclusive execution through conditional store;
cancellation remains checked even when handoff is deferred. Unsupported atomic
patterns, blocking imports and critical-section contention still stop explicitly.
Per-thread PPC contexts, PCR/KTHREAD identity, host FP state and trace storage
remain separate. One process-wide TLS slot bitmap serves independent value banks;
free/reallocation preflights and clears the slot across all registered banks.

Worker failures preserve their original cause and thread trace. A distinct
internal cancellation exception prevents diagnostic memory-inspection catches
from swallowing shutdown. Cleanup releases the main permit, stops and drains
active guest execution, requests worker cancellation, joins all workers while
their registry/dependencies remain alive, then destroys resources. A closed guest
handle still does not terminate a live worker. Native handles close on final
owned destruction.

## Verification and next dependency

All32native tests and133Python tests passed with the actual-ROM environment
enabled and no skips. Scheduler tests cover FIFO exclusion/fairness, target
admission, cancellation with deferred handoff, queued wakeups, first-failure
preservation and draining an active owner before shutdown. Native/guest thread
tests cover real resume counts, output guards, cancellation/join and retained
handle ownership; TLS tests cover independent banks and shared slot reuse.

Logs: `out/worker-runtime-build.log`, `out/worker-runtime-incremental-build.log`,
`out/worker-runtime-ctest.log`, `out/worker-runtime-python.log`,
`out/worker-runtime-dependencies.log`. Independent specification and quality
reviews passed after correcting the shutdown drain race and admission semantics.

Next implement NtWaitForSingleObjectEx with a retained native wait handle,
cancel-aware Windows wait, permit release while blocked and reacquisition before
guest output/state access. Closing a synchronization handle during an outstanding
wait must not invalidate the underlying Windows handle. This remains necessary
runtime work toward the original title/menu, not evidence that the goal is done.
