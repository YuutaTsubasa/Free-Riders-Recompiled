# Original DDS copy memory protection queries

Checkpoint 3ebc65d executes the original DDS handler and enters its copy routine.
Original 825F8750 queries destination 40001BA0 and source 84099F00 via
MmQueryAddressProtect at 82ACB6DC. This read-only API returns protection bits,
not an NTSTATUS or a successful copy result.

Use owned virtual/physical allocation metadata for their supported arenas.
Virtual PAGE_READWRITE reservations retain protection4 even before commit;
physical allocations retain4 or404. An unowned free page returns0, but foreign
maps and unsupported address classes must stop explicitly. Preserve allocation
publication guarantees and leave bytes, accessibility and usage unchanged.

For image addresses, derive immutable ranges from the verified original XEX
security page descriptors, not PE section names or diagnostic host RW backing.
Code/read-only descriptors map to2 and data descriptors to4, matching the pinned
Xenia loader and query ABI. Validate header bounds, image identity/range, page
size/profile and complete descriptor coverage before querying. The actual final
64KiB descriptor covers84090000..840A0000 as READONLY_DATA; source query yields2.

Implement focused RED/GREEN allocator and malformed-image metadata tests,
bind only the exact import/name, then run the original entry and inspect the
next dependency without inventing parser return, upload or display results.
Independent spec/quality review and full native/Python regressions precede a
local checkpoint. Title/menu remains unachieved.
