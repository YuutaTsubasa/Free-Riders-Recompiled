# Native notification listener

Continue from31f0f2c, whose realboot stops at XamNotifyCreateListener82ACB40C.
Original824D0D20 sets maximumversion6 and preserves caller's uint64 mask1.
Original object stores handle at+32480, later polls XNotifyGetNext and closes
through NtClose. The immediate next call is XNotifyPositionUI(2).

Implement owned queue-backed listeners with genuine nonsignaled manual-reset
Windows events. Preserve mask/version filtering, FIFO and firstmatching-ID
dequeue, output preflight before consuming, optional parameter output, and
close/deregister ownership. Keep events signaled exactly while queue nonempty.
Use the existing native sync registry/wait-duplicate lifetime, with a separate
notification kind rejecting ordinary event mutation and async-file completion
capabilities. Serialize queue/producer operations; all GuestMemory and registry
access stays under guest execution ownership. No invented startup notifications.

Root owns typed native sync additions, CMake/builds/bindings/integration/docs/git.
Existing runtime agent owns notification queue and its tests, RED before GREEN;
independent review follows while root runs original entry and regression suites.
Tests must prove real event signal/wait behavior, masks including high64bits,
version and ID filtering, guard-safe transactional output/dequeue, stale/wrong
handles, pending queue limits, close/destructor and retained waiter lifetime.

Do not adopt Xenia's heuristic UI/sign-in/input startup injection. Future native
input/profile/UI producers must emit observed transitions with audited payloads.
Do not claim notification queue construction proves title/input/menu rendering.
PositionUI policy requires real retained presentation configuration, not a no-op
claim that UI exists. Audit in out/notify-listener-next-audit.md records the
original instructions/hashes and pinned reference limitations.

Completed: typed native event RED/GREEN; NativeNotifications RED/GREEN;
placement setter RED/GREEN; original boot missinglistener regression RED/GREEN.
The real constructor saves returned7210005C at object70137D40+7EE0, retains
bottom-center placement2 and then calls XamUserGetSigninState82ACB32C from
LR822344D8 (single user-index argument0; r4 is not flags). All60 CTest/192 Python tests pass without skips.
Independent primitive and expanded queue/binding review found no blocking
defect. No native notification producer is attached yet; game poll/close has
not executed in the observed boot. These are covered by isolated queue tests,
not claimed as original-game execution. Full title/menu goal remains open.
