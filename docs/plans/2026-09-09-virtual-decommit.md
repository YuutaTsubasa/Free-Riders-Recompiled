# Native virtual decommit for original heap trimming

Commit 81d8f91 completes original 64x64 and 128x128 texture creation. During the
256x256 transfer, original 824E0CB0 requests NtFreeVirtualMemory at 82ACB98C with
base 40070000, size 10000, type 4000, debug 0; outputs reside at 70130A94/70130A90.
Implement actual native decommit, preserving guest reservation/protection and
original accounting. Addresses and request fields here are hexadecimal.

Support the observed exact MEM_DECOMMIT mode on owned 4K/64K virtual arenas.
Validate both output words, nonoverlap, no overlap with the rounded target,
aligned base, nonzero size and 64-bit rounding/ownership before effects. On
success return unchanged base plus heap-page-rounded size. Other modes remain
explicit missing behavior.

GuestMemory decommit removes logical committed spans but retains reservations
and private host allocation metadata. Prepare extent splits before mutations;
reject atomic reservation/import/provider/WC overlap. Actually decommit private
Windows pages, skipping untouched placeholders; preserve neighboring bytes.
Recommit existing private backing with MEM_COMMIT, zeroing discarded pages
without clearing live pages. POSIX must discard backing and restore PROT_NONE
while retaining owned address space. Fatal partial host failures cannot claim
rollback of discarded bytes or return success.

Test RED/GREEN native page state, partial split/repeat/reserved-only ranges,
budget reclamation, zeroed recommit/preserved neighbors, ownership/output guard
failure atomicity, page rounding/status/protection, then rebuild and run original
entry. Full regression, independent review and local commit follow; original
title/menu remains unachieved.
