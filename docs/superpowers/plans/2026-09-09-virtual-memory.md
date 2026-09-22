# Virtual memory initialization implementation plan

> **For agentic workers:** Use superpowers:subagent-driven-development for the memory component and independent reviews; the primary agent integrates and verifies actual native execution.

**Goal:** Implement the reserve/commit behavior required by the observed NtAllocateVirtualMemory call, then capture the next real initialization stop.

**Architecture:** Extend GuestMemory to distinguish reserved guest ranges from committed accessible bytes, retaining all existing import and bounds guards. A bounded VirtualMemory service owns deterministic 4 KiB and 64 KiB arenas, checks the Xbox five-argument ABI, and writes base/size outputs only after success. The native dispatcher resolves only the verified import name/address and logs requests/results.

**Tech Stack:** C++20, native CTest, Python integration tests, existing Clang/XenonRecomp diagnostic sources.

## Scope and contract

The observed call in sub_824E1018 at return address 0x824E1364 uses r3/r4 as base/size pointers, r5=0x60002000 (MEM_HEAP | MEM_LARGE_PAGES | MEM_RESERVE), r6=4 (PAGE_READWRITE), and r7 for the debug flag. Record actual pointed-to values before implementing dispatch. The original user approved advancing runtime services; no new product decision or platform scope is introduced.

Supported scope: zero-base allocation and page-aligned fixed-base requests in bounded diagnostic arenas; reserve, commit and reserve+commit; 4 KiB and 64 KiB pages; read/write protection; optional heap/nozero/top-down flags. Reservations remain inaccessible until committed; recommit preserves existing bytes; new committed memory is zero-initialized. Return explicit NTSTATUS for invalid size, exhausted arena and conflicting reservation. Unsupported page/protection/debug/reset modes stop diagnostically instead of falsely succeeding. No free/protect/query service is silently provided.

## Tasks

- [x] Add and run failing native tests for reserve/commit separation, adjacent committed spans, recommit preservation, overlap and bounds before changing GuestMemory. Add synthetic VirtualMemory tests for in/out guest pointers, rounding, disjoint allocation, 64 KiB heap flags, fixed commit, exhaustion/conflict, invalid pointers and unsupported modes. Implement `src/virtual_memory.{h,cpp}` with bounded arenas and preserve existing fixed image mappings.
- [x] Add a failing native boot expectation for allocation request logging. Log the known five arguments while retaining the old import stop, rebuild incrementally and record the actual request. Tighten the service scope against observed values.
- [x] Add an integration expectation that the old allocation stop is passed; wire VirtualMemory to exact `__imp__NtAllocateVirtualMemory` at 0x82ACB99C. Build and record reserve/commit requests, returned ranges and the first new stop. Tighten regression to the actual observed next stop; do not infer gameplay success.
- [x] Register memory tests in CMake and build_tools.ps1, run full Python integration suite and CTest with verified local XEX/image paths, and check pinned dependencies. Independently review specification and quality; fix concrete findings.
- [x] Document supported scope, exact trace and remaining gaps in README/progress and a virtual-memory contract note. Verify ignored game data and clean staged diff, then commit project sources only.

Reference: Xenia revision 95a5c3ee250f80c3b9d139658649d9ffb6db3eec, `src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc` and `src/xenia/xbox.h`. Continue the existing development branch to reuse verified local assets and generated outputs; initial working tree is clean.

## Verification outcome

Reserve 1 MiB and commit 64 KiB at 0x40000000 both succeed. New stop: KeGetCurrentProcessType at 0x82ACB59C, LR 0x824E14B8, calls=8. Final suite: 82 Python tests, 3 CTest targets, zero skips. Specification and quality reviews approved.

An existing Windows publication flake recurred during full verification. Controlled probes observed transient WinError 5 recovering after 50 ms. Added bounded Windows rename retries and four fault-injection regressions; 800 repeated diagnostic-generation tests pass. The locking process remains unidentified; see docs/progress.md for failure and probe logs.
