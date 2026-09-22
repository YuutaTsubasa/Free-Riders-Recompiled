# Original thread references, priority and affinity

Historical checkpoint; latest runtime evidence: [original worker execution](original-worker-execution.md).

The original title/menu goal is **not achieved**. This increment runs the
original thread configuration wrappers and reaches their first resume request.
Neither original worker has executed yet; no original game draw, Present or
title/menu transition is established.

## Actual execution

Build using `scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory
out/recomp/diagnostic-lhzu`, then:

```powershell
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets
```

`out/thread-policy-boot.log` records two real native threads created by original
startup, with guest handles `72200004` and `72200008`, guest IDs2/3, independent
PCRs `73000000`/`73200000`, and original XAPI startup `824DD0A8`. Both original
contexts dispatch virtually to `824B2320`; those targets are observed, not called.
The original wrappers then perform these operations on each real owned thread:

| Object | Guest priority increment | Actual prior/current host priority | Guest previous/new CPU mask | Actual host mask |
| --- | --- | --- | --- | --- |
| `73001000` | -1 | 0 / 0 | 1 / 8 | 8 |
| `73201000` | +1 | 0 / 0 | 1 / 8 | 8 |

Each reference-by-handle returns the actual prepared KTHREAD and increases its
explicit reference count to1; each original dereference returns that count to0.
The new actual stop is:

```text
IMPORT_CONTEXT name=__imp__NtResumeThread address=0x82acb54c lr=0x824d011c sp=0x70131400 r3=0x72200008 r4=0x70131450 r5=0x701313c4 r6=0xffffffff824dd0a8 r7=0x1 r8=0x1 r9=0x1 r10=0x0
STOP import-function @0x82acb54c: __imp__NtResumeThread
LAST_FUNCTION sub_824D0108 @0x824d0108 LR=0x824d011c calls=2004
```

The expected diagnostic exit code is3. Native IDs and host masks depend on the
actual process and machine. Both creation records show suspended1/entry_started0.

## Semantics and limits

The thread type export at `820007E8` is a checked read-only identity `72300000`.
That address is reserved without a readable object descriptor; unsupported
descriptor access still stops. Handle/type validation resolves the existing
owned thread, with invalid-handle/type-mismatch statuses and checked big-endian
output publication. No dummy thread object is returned. Reference semantics are
based on the pinned [Xenia object bridge](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_ob.cc).

Guest handle closure prevents further handle references. It does not cancel a
live parked thread: execution lifetime is distinct from handles and explicit
references. Until guest exit is implemented, native execution ownership remains
with the session and teardown cancels parked entries before invocation.

Priority applies checked Windows GetThreadPriority/SetThreadPriority operations.
The guest-to-host mapping follows pinned
[Xenia XThread](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xthread.cc):
increments greater than34/17 map to2/1; below-34/-17 map to-2/-1; otherwise0.
The returned previous value is the actual host relative priority. This is the
reference's compatibility policy, not proof of exact Xbox scheduling equivalence.

Single guest CPU masks select the corresponding bit of a cached allowed host
process mask, modulo the available CPU count on smaller systems. The process
must belong to one processor group; zero masks or group ambiguity fail explicitly.
This modulo policy is a host adaptation. Initial creation pins the parked thread
before publishing outputs. Subsequent changes use real SetThreadAffinityMask,
preserve the previous **guest** mask in the output, and update PCR/KTHREAD CPU
fields only after validating all writes and host success. Output/CPU-field
overlap and unsupported multiple-bit masks are rejected. Windows API group
constraints: [GetProcessAffinityMask](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getprocessaffinitymask),
[SetThreadAffinityMask](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setthreadaffinitymask).

Guest resume is still unsupported. Shared guest runtime state, reservations,
TLS allocation, critical-section contention, object registries, worker failure
propagation and shutdown must support worker execution before it is connected.
The actual next request resumes handle72200008, not the first created worker.

## Verification

The focused native-thread and guest-thread tests passed. Added coverage checks
real queried priority and affinity, previous values, six guest CPU mappings,
balanced references, type mismatches, invalid/closed handles, live ownership
after close, preflight failures and overlap rejection. The boot regression
requires both original workers and configuration paths before the exact resume
stop. All31native tests and132Python tests passed with the real-ROM environment
enabled and no skips. Pinned dependencies also passed verification. Logs:
`out/thread-policy-build.log`, `out/thread-policy-ctest.log`,
`out/thread-policy-python.log`, `out/thread-policy-dependencies.log`.
