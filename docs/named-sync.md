# Original named event and skeleton worker

The original entry now creates `Event_NuiGetSkeleton` as a real Windows manual
reset event, initially nonsignaled. The original program clears it, creates and
resumes its skeleton worker, and that worker reaches a real event wait. No signal
is synthesized to make initialization progress. There is still no original
Draw/Present, title screen or keyboard/controller transition to the menu.

## Guest ABI and native ownership

The audited request is `NtCreateEvent` at `82ACB55C`, returning to `824D01FC`:
output `70131640`, attributes `70131650`, event type 0, initial state 0.
The original helper builds a 12-byte big-endian attributes structure containing
root `FFFFFFFC`, a pointer to an 8-byte ANSI descriptor, and flags `80`.
The source name has Length 20 and MaximumLength 21.

The adapter checks the writable four-byte output, all attributes, the descriptor
and exactly Length name bytes before creating an object. It snapshots the name
before output publication, including when output overlaps input. MaximumLength
and the trailing terminator are not read. Unsupported roots, flags, empty names,
path separators and non-printable/non-ASCII names stop explicitly.

The registry owns unnamed Windows events/semaphores and a separate guest name
index. Exact-name, same-kind reopen retains the same guest handle, increments its
open-reference count and returns `40000000`; the existing state and reset mode
or semaphore limits remain unchanged. Reopen consumes no new handle ID. A
different kind or a case-only spelling collision stops explicitly. This limited
case policy avoids asserting an unverified Xbox case-comparison rule.

Each successful close releases one guest open. Final close removes the name and
owned Windows handle; an already retained wait continues through its independent
native duplicate. Creation rolls back both indexes and closes the new native
handle if publication fails. Final close resolves its name index before closing
the native handle, so an allocation failure cannot leave a stale closed handle
registered. Namespace access follows the existing serialized guest execution.

## Actual original continuation

```powershell
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-eqv
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets --game-region=ntsc-us
```

`out/named-sync-boot.log` records event handle `72100038` and subsequent original
thread creation: guest ID 11, handle `72200028`, startup `824DD0A8`, worker
`824395E8`, argument `701327E0`, PCR `74200000`, KTHREAD `74201000`, TLS
`74202000`, stack `74221000..74261000`. The game assigns affinity mask 4 and
resumes the initially suspended native thread. The actual worker clears the
event and waits on `72100038` with a null timeout pointer.

The main thread continues to `ExGetXConfigSetting(3,14,buffer,1,required)` at
`82ACB5FC`, return address `824D1BD4`: buffer `701311D0`, required-size cell
`701311D2`. It stops because this user-country setting is not implemented.
The configured game region remains `00FF`; that is separate from user country.

## Verification

Both new suites failed against the missing implementation in
`out/named-sync-red.log`; the actual-entry regression also failed before the
named event was available (`out/named-sync-entry-red.log`). The final build
passed **47 native tests and 180 Python tests**, with no skipped Python tests:
`out/named-sync-ctest.log` and `out/named-sync-python-final.log`.

Coverage includes shared state, retained opens, close/recreate, independent wait
lifetime, case/type collisions, host namespace isolation, native handle cleanup,
guest guards, exact output width, overlapping input/output and counted-name
boundaries. The real-entry test now requires the additional original worker and
its event wait; the intermediate failure on the old nine-worker expectation is
preserved in `out/named-sync-python.log`. Parser, native lifetime and ABI changes
were independently reviewed. The checkpoint binds the executable hash, commit,
command, event/worker evidence and actual next stop.

## Sources

- Original helper `824D2898..824D28F0` SHA-256:
  `7f74233a9adf8bb4fe3f8a822b80016569e61ae82b470c99443acd37e19e5daa`;
  wrapper `824D01A8..824D023C` SHA-256:
  `aa182a847fdedf1217141256c6ab06458c92507179c19967c8ef340762c2089f`.
  Local audits: `out/named-sync-next-audit.md/json` and
  `out/named-sync-continuation-audit.md/json`.
- [Pinned kernel named-object lookup and retained-handle model](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc).
- [Windows event state and wait behavior](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createeventw).
- [Temporary object name and reference lifetime](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-zwmaketemporaryobject).
- [Pinned user-country query size and identifiers](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_xconfig.cc).
