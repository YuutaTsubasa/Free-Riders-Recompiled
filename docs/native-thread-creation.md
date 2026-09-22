# Original suspended worker creation

Historical checkpoint; latest runtime evidence: [original worker execution](original-worker-execution.md).

The title/menu goal is **not achieved**. The original game now creates its first
real Windows thread, with the suspended state requested by its own code. No
worker entry, game draw, Present, title screen or input-to-menu transition has
executed at this checkpoint.

## Reproduction and actual result

Build with `scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory
out/recomp/diagnostic-lhzu`, then run:

```powershell
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets
```

`out/suspended-thread-boot.log` records the original entry `824D22F0`, real asset
reads, shader/effect initialization, vertex declarations and synchronization
objects followed by:

```text
RESULT ExCreateThread output=0x70131360 handle=0x72200004 status=0x0 startup=0x824dd0a8 worker=0x824b1e58 argument=0xffcfd240 flags=0x1 pcr=0x73000000 thread=0x73001000 tls=0x73002000 stack_limit=0x73021000 stack_base=0x73025000 guest_id=2 native_id=34524 suspended=1 entry_started=0 backend=windows-thread
ORIGINAL_WORKER_TARGET context=0xffcfd240 vtable=0x821aa704 target=0x824b2320
STOP import-variable @0x820007e8: __imp__ExThreadObjectType
LAST_FUNCTION sub_824CFE68 @0x824cfe68 LR=0x824b1db0 calls=1945
```

The native ID varies each run. Exit code 3 is the intentional stop on an
unsupported dependency. The logged virtual target was read from the actual
worker context; it has not been invoked.

## Ownership and execution semantics

`NativeThread` uses Windows `CreateThread` with `CREATE_SUSPENDED` and a separate
16 MiB host stack reservation. Its tested resume/join path uses the real Windows
operations, preserves the exit code, and propagates callback exceptions. Cleanup
requests cooperative cancellation, resumes a never-started thread only so its
trampoline can exit before calling the guest, joins it, and closes its handle.

`GuestThreads` prepares independent PCR, KTHREAD, static/dynamic TLS, guest ID and
guarded guest stack. The observed guest stack request `1000` rounds to `4000`;
zero uses the executable's actual default `40000`. Known fields and startup
arguments follow the pinned Xenia reference
`95a5c3ee250f80c3b9d139658649d9ffb6db3eec` (`xthread.cc`, `xthread.h`,
`xboxkrnl_threading.cc` and `threading_win.cc`). Unsupported process-object fields
remain guarded. The callback owns a distinct PPC context and preserves the
original XAPI startup with `r3=worker`, `r4=argument`; host FP state is initialized
on the actual worker only if execution becomes supported.

Both output words, targets, flags, stack limits and slot collisions are checked
before allocation. Outputs are rechecked after the entry factory and native
creation, before publishing either result. Failed factories/publication expose
no successful handle and cancel any created native worker. Reserved guest slots
are retired for this diagnostic session because GuestMemory has no release API.

Guest `NtResumeThread` is deliberately still unsupported. Current shared runtime
tracing, memory metadata, reservation state, TLS allocation, critical-section
contention and object registries must support concurrent guest execution before
that import can resume a worker. Host primitive tests do not prove guest worker
execution. The immediate original dependency is the thread object type and
reference/priority/affinity path preceding resume.

## Verification

All 31 native tests and 132 Python tests passed with the real-ROM environment
enabled and no skips. New tests cover native suspension, actual foreign-thread
execution, exit/exception propagation, cancellation/handle ownership,
independent guest state/TLS, stack guards, invalid inputs/templates, allocation
collisions, retired failed slots and atomic output preflight. The boot regression
requires the original creation result and the exact next dependency above.

Logs: `out/suspended-thread-build.log`, `out/suspended-thread-ctest.log`,
`out/suspended-thread-python.log`, `out/suspended-thread-dependencies.log`.
These establish this runtime increment, not the title/menu acceptance goal.
