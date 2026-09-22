# Whole virtual reservation release for original DDS cleanup

Commit 98b6694 passes four original texture creations and two native decommits.
The fifth 1024x1024 transfer completes, but its DDS cleanup requests MEM_RELEASE
at base 40260000 with input size 0. The owned reservation is 410000 bytes. Keep
the original destructor, heap unlinking and result handling intact.

Implement exact 8000/debug 0 with zero input size in VirtualMemory::free.
Preflight both full writable output words and exact service-owned base, reject
output overlap against the full reservation, and return unchanged base plus full
reservation size. Remove service ownership only after native release succeeds.
Other free modes and nonzero-size release remain explicit unsupported profiles.

GuestMemory::release requires an exact logical reservation. It prepares all
metadata removals and host operations before effects. Reject atomic reservation,
import, computed-provider or write-combined overlap. Preserve the 4 GiB host
address-space envelope: Windows restores every fully contained private allocation
to a placeholder, including partially decommitted allocations; untouched
placeholders remain reserved. POSIX replaces the owned host-rounded span with
anonymous PROT_NONE. Irreversible partial failures stop without fake rollback.

Tests first fail on a stub, then verify reservation/commit accounting, zeroed
reuse, exact-base/size guards, full-target output aliases, adjacent data,
multiple backing allocations, partial decommit, reserved-only spans, native
placeholder state and destructor cleanup. Rebuild the native diagnostic, run the
original entry, inspect the next actual stop, run full regression and independent
review, then save a local source commit and same-run evidence. Title/menu remains
unachieved until its full original rendering and input transition are verified.
