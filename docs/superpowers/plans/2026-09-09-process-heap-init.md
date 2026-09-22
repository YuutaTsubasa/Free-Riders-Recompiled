# Process and heap synchronization initialization plan

> **For agentic workers:** Use superpowers:subagent-driven-development for the bounded critical-section initializer and independent reviews. The primary agent owns process dispatch and native integration.

**Goal:** Advance heap initialization past current-process-type lookup and the observed critical-section initialization without faking synchronization operations.

**Architecture:** The diagnostic hosts exactly one user process (type 1). Only the verified KeGetCurrentProcessType import reports that state. If the subsequent RtlInitializeCriticalSection is reached, initialize the confirmed guest fields using checked big-endian memory, preserving unspecified bytes; enter/leave/wait/spin operations stay explicit stops.

**Tech Stack:** C++20, native CTest and Python integration, pinned XenonRecomp generated source and Xenia ABI references.

- [x] Verify Xenia kernel_state process-type initialization, both reference recompilations and actual Free Riders call site. Add a failing integration assertion, implement exact getter name/address dispatch, build and record actual next stop.
- [x] For the observed critical-section import, add synthetic tests before implementation: exact field writes, byte order, preserved unspecified fields, complete 28-byte prevalidation, no partial modification on invalid or guarded ranges. Implement a small `src/critical_section.{h,cpp}` initializer, with no locking-success stubs.
- [x] Integrate only the observed RtlInitializeCriticalSection address/name pair, preserving register state for the void ABI. Build and execute the actual entry; assert the next exact stop and any useful heap data evidence.
- [x] Run all Python tests and CTest; independently review spec and quality, fix concrete findings, update progress and contract documentation, then commit source/config/docs only.

Reference revision remains Xenia 95a5c3ee250f80c3b9d139658649d9ffb6db3eec (`kernel_state.h/.cc`, `xboxkrnl_threading.cc`, `xboxkrnl_rtl.cc`). Free Riders imports KeGetCurrentProcessType at 0x82ACB59C, with its observed return address 0x824E14B8. The generated next candidate is RtlInitializeCriticalSection at 0x82ACB49C. Continue the existing clean development branch and retained local artifacts; no new user preference is required.

Result: initializer executes at guest address 0x40000618. Actual entry now stops at XboxHardwareInfo (0x82000798), LR 0x824DD01C, calls=20, exit=3. Windows build, 82 Python tests with zero skips, four CTest targets and pinned-tool verification pass. Independent spec and final quality reviews approved. Source generation and game assets are unchanged. The next investigation is the hardware-info pointer and the flags word consumed by sub_824DF608; it is not implemented in this increment.
