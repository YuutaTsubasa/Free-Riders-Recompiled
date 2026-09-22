# Diagnostic virtual memory contract

The native Xbox import `__imp__NtAllocateVirtualMemory` at `0x82ACB99C` uses five arguments: guest base-address pointer (r3), guest region-size pointer (r4), allocation flags (r5), protection flags (r6), debug-memory flag (r7). NTSTATUS returns zero-extended in r3. Base and size are 32-bit big-endian in/out words; other argument registers remain unchanged.

The first observed call from `sub_824E1018`, with LR `0x824E1364`, requests base zero, size `0x100000`, type `0x60002000`, protection `4`, debug `0`. This reserves a 1 MiB range using 64 KiB pages and the heap flag. The diagnostic records the actual request in `out/vm-before.log` before enabling the implementation.

The implemented service returns base `0x40000000` and size `0x100000`. The game then requests commitment of `0x10000` bytes at that base with flags `0x60001000`. This also succeeds, after which guest heap initialization writes its metadata into the committed range and reaches `KeGetCurrentProcessType` at `0x82ACB59C` (LR `0x824E14B8`). That was the stop at commit `48b25ea`; the subsequent [process/initializer increment](process-heap-init.md) advances to `XboxHardwareInfo`. This is progress within initialization, not a playable game.

## Memory state

GuestMemory reserves a 4 GiB host address space once. Guest reservations and host-accessible committed ranges are tracked separately. Reserving a guest range prevents conflicting allocations but does not allow reads or writes. Committing a reserved subrange makes only those logical bytes accessible. New committed storage is zero-initialized by the host; committing it again preserves its contents. Bounds checks cover adjacent committed spans and continue to reject import variables and unmapped gaps.

The virtual allocation service uses bounded diagnostic arenas: 4 KiB pages in `[0x10000000, 0x20000000)` and 64 KiB pages in `[0x40000000, 0x50000000)`. These are diagnostic allocation choices, not a claim to reproduce Xbox physical memory layout or available hardware memory. Fixed image, PCR, stack and module mappings remain separate. Existing reservations are checked before a new range is claimed. A 4096-reservation metadata budget produces an explicit diagnostic stop; sorted gaps avoid repeatedly scanning every owned page.

## Supported requests

- Reserve, commit, or both; a zero-base commit chooses a fresh reserved/committed range.
- Page-aligned fixed base within an owned reservation for commitment; fixed base selects its arena's page size.
- Size rounded upwards to the selected page size using checked arithmetic.
- Read/write protection, heap, nozero and top-down allocation flags. Nozero permits initially zeroed storage; it does not erase existing committed contents.
- Guest output words updated only on success. Null/zero-size and invalid arena requests return `STATUS_INVALID_PARAMETER`; arena exhaustion and reservation conflicts return `STATUS_NO_MEMORY`.

Unsupported debug, protection, reset, 16 MiB page and unaligned fixed-base modes stop with a diagnostic instead of being accepted silently. High-bit sizes are not interpreted as negative allocation requests. Release, decommit, query and protection imports remain separate, unimplemented services. This is a single-threaded diagnostic service with bounded metadata; it is not a completed kernel memory subsystem. The verified Windows host uses 4 KiB host pages; hosts with larger pages may stop on small-page requests and have not been validated.

## References

Runtime ABI, flag values and existing-reservation behavior were checked against [Xenia memory imports](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc) and [Xbox constants](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/xbox.h) at the already recorded reference revision. The diagnostic deliberately supports a narrower request set and stops on the remaining modes.
