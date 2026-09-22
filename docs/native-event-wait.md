# Original worker native event waiting

Historical checkpoint at `18e0bd4`. Current doubleword reservation progress and graphics stop: [doubleword reservations](doubleword-reservations.md).

The title/menu goal remains **unachieved**. The original startup now passes the
previous worker wait at `NtWaitForSingleObjectEx` (`82ACB63C`) and reaches a new
CPU translation stop at `8274F4B8`. No original draw, Present, title, or menu-input
transition has occurred.

## Same-run evidence

`out/native-event-wait-boot.log` records the original `_xstart824D22F0`, existing
asset/shader/device initialization, five Windows worker creations, and four
original XAPI worker executions. Created native IDs match executing host IDs:

| Guest ID | Guest handle | Native ID | Original work context | Execution |
|---|---|---|---|---|
| 2 | 72200004 | 40204 | FFCFD240 | Remains suspended |
| 3 | 72200008 | 13400 | FFCFD1B0 | Original XAPI/worker, 8 ms event wait |
| 4 | 7220000C | 38656 | FFCFD150 | Original XAPI/worker, infinite event wait |
| 5 | 72200010 | 29936 | FFCFD060 | Original XAPI/worker, infinite event wait |
| 6 | 72200014 | 10228 | FFCFD000 | Original XAPI/worker, 2 ms event wait |

All executed workers follow `824DD0A8 → 824DCE98 → 824B1E58 → 824B2320` using
their own PCR/KTHREAD/TLS/stack. Guest 6 actually returns native timeout and
continues its original loop before waiting again:

```text
RESULT NtWaitForSingleObjectEx guest_id=6 handle=0x72100018 status=0x102 milliseconds=2 infinite=0 backend=windows-sync
STOP unsupported-function @0x8274f4b8: sub_8274F4B8: unsupported_reservation; unchecked_guest_memory
LAST_FUNCTION __savegprlr_29 @0x82a5606c LR=0x82750a50 calls=2784
```

Exit code is 3. Native thread IDs, timeout completion counts and total function
entries vary with host scheduling; regressions check the causal original-worker
sequence and next stop rather than require this particular timing. Infinite
waits cancel during shutdown and all workers join before runtime dependencies
are destroyed.

## Implementation and bounds

The guest adapter validates mode 0/1 and non-alertable waits. A null timeout means
infinite; otherwise a checked big-endian signed 64-bit value supplies zero or
negative relative 100 ns ticks. Conversion rounds upward to milliseconds without
signed overflow, including `INT64_MIN`. Large finite durations are split into
chunks at most `FFFFFFFE` ms, preserving `FFFFFFFF` exclusively for infinite wait.
Absolute deadlines, APC/alertable waits and other object namespaces still stop
explicitly. Invalid synchronization handles return `C0000008`.

Preparation duplicates the actual native event/semaphore handle under the guest
execution permit. The blocking callback uses only the retained duplicate, copied
timeout and stop token. It never accesses guest memory or the mutable object
registry. Another guest may close the original handle without invalidating the
pending wait. Each call owns a cancellation event; a stop callback signals it,
and `WaitForMultipleObjects` selects cancellation at index zero before the target
when both are ready. Callback destruction precedes cancellation-handle closure.
Actual success/timeout remains separate from session cancellation.

`GuestExecution::Lease::run_blocking` reserves the blocked guest ID and host
thread, releases execution ownership, then reacquires through the FIFO before
normal return or callback exception propagation. A cancelled operation cannot
resume guest access. Stop callbacks run outside the scheduler mutex. Shutdown
drains active guest execution and joins native workers while dependencies live.

ABI evidence: pinned [Xenia threading implementation](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc),
[NT wait timeout semantics](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-kewaitforsingleobject),
[DuplicateHandle lifetime](https://learn.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-duplicatehandle),
and [Windows wait ordering/state consumption](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitformultipleobjects).

## Reproduction and verification

From the repository root with already prepared local assets:

```powershell
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-workers
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets
```

The generated source is unchanged from the previous worker-execution build.
Root verified deliberate RED failures for the retained native wait, blocking
execution gate and guest timeout adapter before implementation. Completed checks:

- `out/native-event-wait-final-build.log`: native build succeeds.
- `out/native-event-wait-ctest.log`: 33 native CTest targets pass, including actual
  wait/timeout/consumption, cancellation, duplicate survival, no handle leaks,
  FIFO handoff, blocked identity, error and shutdown behavior.
- `out/native-event-wait-python.log`: 133 Python tests pass without skips, with
  real ROM/assets and native startup enabled.
- `out/native-event-wait-dependencies.log`: all three pinned dependencies verified.

Independent specification and code-quality reviews approved the implementation.

Next inspect the actual reservation/memory instructions in `8274F4B8` and extend
checked CPU translation only with matching original-byte and semantic evidence.
