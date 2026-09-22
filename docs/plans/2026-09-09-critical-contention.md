# Actual original critical-section contention

Observed during genuine boot in `out/user-country-boot.log`: guest thread3 owns
`0xFFCFFFD4` as `0x73201000`, while main `0x70000BB0` attempts entry after a valid
GuestExecution handoff. The old single-thread helper intentionally stops here.
This remains unresolved; country translation progress does not fix the race.

Read-only audit: `out/critical-contention-next-audit.md`, pinned Xenia
`95a5c3ee250f80c3b9d139658649d9ffb6db3eec` xboxkrnl_rtl/threading sources.

Implement a coordinator with owned waiter tokens and a queue per guest address.
All guest effects occur under the existing execution lease. Validate/allocate
before admission increments lock_count; release the lease only for a cancellable
host-only wait. Final leave clears owner, decrements lock_count and selects one
admitted waiter. Finalize ownership after the wait reacquires the lease. Preserve
owner recursion, no barging during handoff, lost-wake protection and untouched
opaque header/list/spin fields. Do not disable scheduling or invent acquisition.

Maintain free (-1,0,0), owned (recursion-1+waiters,recursion,owner) and handoff
(nonnegative,0,0) states. Reject corrupted or unrepresented states. Global stop
must wake every waiter and return cancellation rather than successful entry;
terminal shutdown must drain safely. Guard live PPC reservations before blocking.

Prove RED then GREEN with deterministic multithreaded tests for the observed
owner/contender sequence, recursive ownership with multiple waiters, exactly one
wake, no lost wake, no barging, independent addresses, invalid/preflight failures,
global cancellation and stop/drain. Run genuine original entry and preserve all
failures instead of retrying until a test appears green. Local review and commit.

Only the original title-to-main-menu execution and input/graphics evidence meet
the active user goal; this synchronization work is an initialization dependency.
