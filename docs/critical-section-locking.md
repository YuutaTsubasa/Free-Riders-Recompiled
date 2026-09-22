# Single-thread critical-section transitions

The diagnostic executes one guest thread. The supported blocking Enter/Leave imports are the verified Free Riders pairs `RtlEnterCriticalSection` at `0x82ACB4AC` and `RtlLeaveCriticalSection` at `0x82ACB4BC`. Both have a void ABI; dispatch preserves the PPC register context.

The owner is the guest thread object pointer stored at PCR+`0x100`, currently `0x70000BB0`. It is distinct from the PCR address in r13 (`0x70000000`) and thread ID1. Dispatch verifies all three against the existing diagnostic layout before changing any lock. The pointer and ID offsets follow pinned Xenia [xthread.h](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xthread.h); [xthread.cc](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xthread.cc) binds the guest object, and [xboxkrnl_rtl.cc](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_rtl.cc) uses that object for ownership.

The lock occupies 28 bytes with counts/owner at16/20/24. Its complete range must be checked, aligned to4, type1 and signal0. Header bytes0–15, including the spin byte and list links, are preserved. No waiting occurs, so the spin byte has no effect on supported transitions.

| Operation | Before: lock count, recursion, owner | After |
| --- | --- | --- |
| First enter | -1, 0, 0 | 0, 1, current thread |
| Recursive enter | n-1, n, current thread | n, n+1, current thread |
| Partial leave | n-1, n, current thread (n>1) | n-2, n-1, current thread |
| Final leave | 0, 1, current thread | -1, 0, 0 |

All fields are stored through checked big-endian GuestMemory. Xenia uses native-endian host atomics for lock_count, and the reference recompilations use native owner values with a comment that guest code does not access them. Those are runtime representation choices. This diagnostic deliberately retains its guest BE scalar representation; the observed initial -1/zero counters do not independently establish the byte order of positive counters. Future guest instructions accessing positive counters need semantic verification with this choice.

Unsupported ownership contention, waiter counts, inconsistent state, zero thread identity, wrong-owner/free leave, recursive overflow beyond INT32_MAX, and invalid/guarded ranges stop before any writes. These are diagnostic boundaries, not claimed Xbox error returns. No host mutex, wait queue, event wakeup or multi-thread scheduling is provided. TryEnter remains unimplemented.

`RtlInitializeCriticalSectionAndSpinCount` at `0x82ACC2DC` initializes the same28bytes but writes the spin byte as `min(255,(spin_count+255)>>8)`. Inputs through `0xFFFFFF00` are supported; higher values stop before mutation because the reference uint32 addition would wrap. This restriction avoids treating a likely overflow artifact as proven console behavior. The pinned Xenia implementation returns status0, while the title's wrapper subsequently replaces r3 with its own Boolean1. Dispatch follows the import's status0 contract and preserves the other guest registers. The observed caller supplies4000, encoded as16. This stores the spin setting; contention still stops rather than executing a spin/wait loop.

The first observed section is static game data at `0x82AD08F0`: header `01 00 04 00`, signal0, self-list pointers `0x82AD08F8`, lock count -1, recursion0 and owner0. The guest's `sub_824DCE98` and `sub_824DCF00` pass this section to Enter/Leave and access a separate list at section+28. The existing initializer remains available for dynamically initialized sections.

Tests cover state cycles, recursive counts, byte representation, two independent locks, preserved static headers and rejection without partial writes. This work does not make initialization complete or the game playable.

At commit `0e3b590`, the actual entry completed Enter/Leave at `0x82AD08F0`, recording owner `0x70000BB0` with recursion1/count0 after Enter and owner0/recursion0/count-1 after Leave. It then reached `sub_824D2108` and stopped at `XexCheckExecutablePrivilege` (`0x82ACB5AC`, LR `0x824D212C`, calls24, exit3). The subsequent [privilege query](xex-loader.md) advances to CRT spin-count initialization.
