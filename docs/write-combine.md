# Windows write-combined guest backing

The actual startup's second physical request is size0x12C0, protection0x404 and alignment0x20. Pinned Xenia `xbox.h` identifies0x400 as `X_PAGE_WRITECOMBINE`; read/write is0x4. The runtime now maps this request to real Windows write-combined backing.

## Host mapping and visibility

Windows [memory protection constants](https://learn.microsoft.com/en-us/windows/win32/memory/memory-protection-constants) permit PAGE_READWRITE combined with PAGE_WRITECOMBINE for private [VirtualAlloc](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc) memory. The flag affects complete host pages. It is incompatible with guard, no-access and non-cacheable modifiers, and interlocked access to such memory is not a supported path here.

GuestMemory exposes a fresh-map operation for Windows x86-64. Its Windows4GiB address window now uses [VirtualAlloc2 placeholders](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc2). Fresh host-page gaps are split into exact ranges and replaced by independent private allocations with their required cache protection. Existing committed pages stay intact. Rounded cache extents are tracked separately from logical access extents. Tests use VirtualQuery to verify the actual host protection; a log label alone is insufficient. Other hosts stop before reserving pages for this operation. This does not add a GPU device or presentation backend.

Each checked scalar store that intersects these pages completes with [MFENCE](https://clang.llvm.org/doxygen/emmintrin_8h.html) before guest execution continues, ordering the writes before subsequent guest loads and stores. Ordinary cached stores retain their current path. The current boundary deliberately provides stronger ordering than a relaxed write-combined stream. Direct writes through raw host pointers are outside this helper; future native backends using such pointers must provide their own appropriate synchronization.

Reserved-word loads and conditional stores to write-combined pages stop before changing reservation state or data. Ordinary logical access bounds, import guards and computed read-only fields remain checked. Attribute transitions are not silently performed: an ordinary recommit touching a write-combined host page fails before changing protections or metadata, including when the logical extent occupies only part of that host page.

## Physical allocation

PhysicalMemory accepts exact protections4 and404. The latter selects the real write-combined map; bounds, alignment, rounding, pointer/null behavior and budget checks are unchanged. No other cache flags or combined modifiers are accepted. Both classes consume the same enforced backing budget and count once as title physical pages. Future aliases must share backing and compatible cache attributes, not copies.

The runtime records successful allocation cache mode alongside its pointer and actual committed bytes. Native memory and physical allocation tests verify host protections, zero initialization, reads/writes, normal neighbors, failures and accounting. Title/menu completion still requires original rendering and input evidence.

## Native reservation investigation

A direct probe found that reserving with PAGE_NOACCESS, then requesting404 at commit time, still produces Protect4. Reserving with404 first causes normal commits to inherit404 as well. VirtualProtect also leaves that reservation's cache mode unchanged. Thus one ordinary4GiB reservation cannot represent both cache classes on this host. out/wc-host-probe.log captures these observations.

A separate probe split placeholders into4KiB ranges, replaced one with normal backing and another with write-combined backing, and confirmed respective Protect4/404 through VirtualQuery. Each partition released successfully. Evidence is out/wc-placeholder-probe.log. This is why the host-reservation architecture changes; accepting the original successful return without checking the resulting attribute would have been incorrect.

[VirtualFree](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualfree) preserves placeholders during splitting and rollback. Newly created mappings are reverted on commit failure, while previously committed data stays intact. Destruction releases the owned partitions; no unrelated address range is freed. Missing VirtualAlloc2 support is an explicit initialization failure rather than a silently cached fallback.

## Guest boot and regression evidence

The actual entry obtained the second allocation at0xF8AFB000, with total physical backing0x07205000 bytes and native write-combined protection. It then initialized two further critical sections and reached VdInitializeEngines at0x82ACBC2C. Exit3; last function sub_825011A0, LR825011DC,1271 function entries. No graphics engine or title frame is claimed yet.

Full regression testing exposed an already-isolated placeholder case: splitting the entire placeholder fails with Windows error487. Direct replacement is required for that exact case, as confirmed by out/wc-placeholder-exact-probe.log. The replacement decision must use AllocationBase rather than VirtualQuery's BaseAddress, which begins at the queried page. The existing top-of-address-space test and new ordinary/WC middle-hole tests cover this distinction. A separate rollback probe confirms private replacement→placeholder conversion succeeds with preserved ownership: out/wc-placeholder-rollback-probe.log.

Final evidence: out/write-combine-build-final3.log, out/write-combine-ctest-final3.log (16/16), out/write-combine-tests-final3.log (103 Python tests, no skips), out/write-combine-boot-final2.log (exit3 at the graphics import), and out/write-combine-mfence.log (native MFENCE instructions). The pinned dependency verification passed. The last edit after the boot run only strengthened the guest-memory test; the runtime executable was unchanged by the final build.

The preceding Python run, out/write-combine-tests-final2.log, had one intermittent failure in the existing transient-publication test: it expected two rename attempts but observed three. That test injects one lock failure, mocks out sleeping, then performs a real Windows rename which can itself be temporarily denied. The complete rerun passed without source changes. This remains a test reliability limitation; it is not hidden by discarding the failed log or changing the assertion.
