# Original synchronization objects on Windows

After checkpoint1d9e059, the complete824BBCE8 body was audited: incoming device
r3 is captured in r30 and eventually stored at83E53700. The device is never
dereferenced or passed to its one child call. That indirect call uses the
original r4 allocator's vtable slot16. The entry guard now admits only this
function with the established native device71600000; original code, allocation
and downstream guards remain intact.

`out/device-capture-boot.log` reaches1901 entries and actual NtCreateSemaphore
82ACB5BC, LR824D09E0. The original wrapper824D0990 passes BE32 output701313A0,
attributes0, signed initial1 and maximum1. It tests NTSTATUS, updates LastError
and returns the original output handle. The containing original object stores
the semaphore handle at+4 and uses vtable821AA9D0.

## Native object and guest contract

NativeSyncObjects owns actual unnamed Windows semaphore and event objects. Creation, wait,
release and close use CreateSemaphoreW, WaitForSingleObject, ReleaseSemaphore
and CloseHandle. Counts are maintained by Windows, not shadow counters. Guest
IDs start72100004, advance by4 and are never reused in the process. Namespace
exhaustion stops before creation. Existing file IDs now stop below72100000 so
the two registries cannot collide. These IDs are opaque values, not guest memory.

Guest creation requires the observed unnamed profile and a nonnull aligned
writable BE32 output. The full output is checked before host allocation.
Invalid counts clear output and return C000000D; valid creation publishes only
an owned native handle. Publication exceptions close the newly created handle.
Nonnull attributes and null output stop explicitly: those broader profiles are
not implemented. NtClose routes synchronization-range values to their owner and keeps
existing file closure. Invalid/stale semaphore IDs return C0000008.

The native primitive supports real wait success/timeout and positive-count
release with actual previous count. Overflow returns C0000047 and preserves
the count. Unsupported host failures stop with their Windows error. This scope
does not establish Xbox zero-adjustment release semantics, guest APCs or
concurrent guest handle-close behavior.

## Sources and pending observed behavior

Pinned [Xenia threading ABI](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc)
and XSemaphore confirm create argument order and zero output on initialization
failure. Local copies are under out/xenia-loader-reference. Xenia accepts null
output and named attributes; this runtime stops those unimplemented profiles.

Microsoft documents [native creation](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createsemaphorew),
[wait](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject),
and [release](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-releasesemaphore).
Over-release leaves the count unchanged. The
[NTSTATUS table](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-erref/596a1078-e883-4972-9bbc-49e60bebca55)
defines C0000047; the [Win32 table](https://learn.microsoft.com/en-us/windows/win32/debug/system-error-codes--0-499-)
defines ERROR_TOO_MANY_POSTS=298.

Original method824B9EC0 calls release with adjustment0 and a previous-count
output. If observed, that Xbox behavior needs separate evidence: pinned Xenia
discards the native release boolean, so it cannot prove correct success here.
Other methods request infinite/timed waits and positive releases. The original
wait wrapper converts unsigned milliseconds to BE64 negative100ns intervals,
with -1 represented by a null timeout pointer. Guest wait/release imports must
be connected only with their actual requested semantics established.

This is an initialization dependency of the [original title/menu goal](title-menu-goal.md).
No original draw, Present, title or input transition is established by semaphore
creation or primitive tests.

The subsequent original call reaches NtCreateEvent82ACB55C at1936entries,
LR824D01FC, output70131330, attributes0, event_type1 and initial_state0. It
requires a real initially nonsignaled auto-reset event. Both kinds share the
same typed native owner, wait/close paths and nonreused ID range. Native event
set/reset primitives are tested without inventing a guest previous-state value.
The original guest event wrapper maps manual-reset to type0 and auto-reset to1;
unknown type values stop as unimplemented profiles. Microsoft
[CreateEventW](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createeventw)
documents the exact wait/reset behavior. Pinned XEvent returns a constant1 from
Set/Reset, so that reference cannot establish guest previous-state semantics.

## Actual continuation and verification

```powershell
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-lhzu
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets
```

`out/event-clear-boot.log` proves the same original-entry run creates semaphore
72100004 (initial1, maximum1) and event72100008 (type1, initial0). NtClearEvent
82ACB6CC calls the real ResetEvent path and returns status0. It has only the
handle argument; other volatile registers are not previous-state outputs.

Next stop is ExCreateThread82ACB77C, LR824D2518, after1944 entries. Observed
arguments are output70131360, stack1000, thread-ID outputFFCFD260, XAPI startup
824DD0A8, worker824B1E58, contextFFCFD240, flags1. This is an actual request for a
suspended worker, not permission to return a fabricated thread or skip its work.
Current runtime still has one guest thread; the native object map and global
runtime context require further design before concurrent guest execution.

Native tests verify real consumption/timeout/release and unchanged counts on
overflow, actual auto/manual-reset behavior, invalid handles and types, monotonic
IDs and native process handle-count cleanup after mixed-kind destruction. Guest
tests verify BE handle publication, unsupported profiles and invalid/protected/
partial output without allocation, count-error output0, and both event types.
Boot testing requires original declarations, real creation/clear results and the
next genuine thread stop. No guest wait/release/set-event import is fabricated.

RED/GREEN evidence: `out/semaphore-native-red-test.log`,
`out/semaphore-guest-red-test.log`, `out/semaphore-native-green-test.log`,
`out/semaphore-guest-green-test.log`, `out/event-native-red-test.log`,
`out/event-guest-red-test.log`, `out/event-native-green-test.log`,
`out/event-guest-green-test.log`. Earlier semaphore-specific target names were
renamed to native_sync_objects/guest_sync_objects as event support was added.

Final source build: `out/event-clear-build.log`. Verification:
`out/native-sync-final-ctest.log`29/29, `out/native-sync-final-tests.log`132Python
without skips, and `out/native-sync-final-dependencies.log`all three pins.
Independent specification and quality reviews approved the bounded native and
guest creation/clear/close scope, registry ownership and device-capture exception.
Original title/menu, draw/Present and confirmation input remain unachieved.
