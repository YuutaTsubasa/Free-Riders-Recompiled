# Single-thread TLS

The CPU diagnostic provides TLS for its one known guest thread. This is initialization support, not a scheduler or a complete kernel thread implementation.

The verified title's XEX optional header `0x20104`, at raw-header offset `0x35F8`, contains four big-endian words: 64 dynamic slots, raw source `0x83E5F800`, static data size156, raw size156. The existing validated XEX lookup supplies the metadata before any guest code executes. Both the image and raw header fingerprints are checked first.

Storage is mapped independently at `0x71300000`: static data first, followed by 64 four-byte slots at `0x7130009C`. Its exact logical size is412 bytes. Raw bytes are copied without endian conversion; the static remainder and slots start zeroed. PCR+0 and the thread object's +`0x68` contain the static base. The thread remains `0x70000BB0`, PCR `0x70000000`, thread ID1. The old placeholder at PCR+`0xAB0` had only256 bytes before the thread object; using it for all412 bytes would overlap that object.

This layout and raw-data initialization follow pinned Xenia [xthread.cc](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xthread.cc); the guest pointer offsets are defined in [xthread.h](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xthread.h). Dynamic words are read and written through checked big-endian GuestMemory, so guest loads/stores and imports share the same storage.

| Import | Verified address | Supported result and effect |
| --- | --- | --- |
| KeTlsAlloc | 0x82ACBCCC | Lowest free slot index; zero the word, then mark allocated |
| KeTlsFree | 0x82ACBCDC | Clear an allocated word, release its index, return1; indexFFFFFFFF returns0 |
| KeTlsGetValue | 0x82ACBCAC | Return the allocated slot's current guest word |
| KeTlsSetValue | 0x82ACBCBC | Store the supplied word to an allocated slot, return1 |

Dispatch requires both the exact name and address. It verifies r13, the current-thread pointer, thread ID and both TLS pointers before invoking a service. Arguments are captured before r3 is replaced with a zero-extended 32-bit result; other registers are preserved.

The return and zeroing contracts follow Xenia [xboxkrnl_threading.cc](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc). Its [kernel_state.cc](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/kernel_state.cc) clears thread values before releasing an index; [bit_map.cc](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/base/bit_map.cc) chooses the lowest free index. This diagnostic has one guest thread and no host synchronization.

The component accepts 1–2048 slots, at most64KiB of static data, and raw size no larger than static size. Invalid metadata and unreadable raw sources are rejected before destination mapping. A destination collision or guarded write cannot silently overwrite existing data. Operations complete their checked guest word access before changing allocation ownership.

These are explicit diagnostic restrictions, not claims about console error handling: Get/Set/Free of an unallocated or out-of-range index stop with `tls-index`; exhausted configured storage stops with `tls-capacity`. Xenia uses a2048-slot process bitmap even when a title's thread storage is smaller; returning a fabricated out-of-memory result at this title's64-slot boundary would obscure that unresolved difference. No out-of-range reference access, bitmap overflow, host-vector expansion or arbitrary-success stub is copied. No last-error side effect is inferred.

Marathon and Unleashed use per-host-thread vectors for values and a global reusable index list. Their implementations were consulted at the pinned revisions in the dependency configuration, but are not used as proof of this game's direct guest-memory TLS layout.

The native component tests cover exact initial bytes and limits, guest/import coherence, allocation/reuse, sentinel and invalid indices, exhaustion, malformed metadata, source range overflow, mapping collisions and guarded operations without ownership loss. The actual boot trace supplies separate evidence for which imports the title has reached; implementation of four services does not mean all four have been exercised by the real startup path.
