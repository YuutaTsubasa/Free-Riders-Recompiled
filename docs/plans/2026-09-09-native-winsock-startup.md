# Native Winsock startup for the original initialization

Continue from16a15ce. Original8278DCF8 allocates496-byte stack, passesSP+80
and preservesr31 atSP+480: the WSADATA output is400bytes. The Xbox field
layout agrees with32-bit WSADATA (vendorpointer396), not Windows64-bit layout.
Do not copy Xenia's unrelated offset400 round-trip over the saved register.

Build native Windows startup with explicit field marshalling, actual negotiated
version/error results, complete guestoutput preflight before native effects,
and one WSACleanup per successful acquisition. Keep native pointers out of guest
memory. Version2 legacy vendor fields are ignored byWinsock; publish no vendor
pointer. Do not emulate sockets, online accounts, or sensor frames in this work.

First implement/test the actual native startup/lifetime independently. Preserve
the original optional Ex lookup flow and decide its export policy from the
separate semantic audit before binding. Do not return a host pointer as a guest
callable or claim unknown Ex SDK semantics are implemented.

Tests: compare to independent nativeWSADATA, exact400-byte range/sentinels,
failed native negotiation preserves outputs/count, failed preflight leaves
native uninitialized, multiple acquisitions and destructor cleanup; never open
network connections. Then integrate, realboot, review and fullverification.
Microsoft WSAStartup/WSACleanup documentation and installed Winsock2.h are the
primary host contracts. Full original title/menu goal remains unproven.

Completed: native lifecycle test RED then GREEN; module-export and original
boot fallback regressions RED then GREEN. Windows max macro collision fixed
with the parenthesized numeric_limits max call. Original ordinal36 lookup now
truthfully reports unavailable; original8270CE98 reaches realWSAStartup with
caller1/version2/output701315F0, LR8270CE9C, actualversion2/highversion202.
Nextstop XamNotifyCreateListener82ACB40C atLR82232E3C. Full57 CTest/192 Python
tests pass without skips; independent read-only review found no bounded defect.
No Ex ABI claim, fake pointers, connectivity or notification events introduced.
