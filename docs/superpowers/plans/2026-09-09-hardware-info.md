# Hardware information implementation plan

> **For agentic workers:** Use superpowers:subagent-driven-development for the isolated guest-data component and spec/quality reviews. Root owns import binding and boot integration.

**Goal:** Advance the verified Free Riders entry beyond XboxHardwareInfo without exposing guessed hardware fields.

**Architecture:** Bind the exact variable import `__imp__XboxHardwareInfo` at `0x82000798` to a four-byte guest flags word at `0x71200000`. Use the pinned Xenia profile flags `0x20`; the guest tests bit `0x10`, which is clear. Only the flags prefix is mapped; offsets 4 onward remain inaccessible, including the known CPU-count field that this path does not need. No hardware device, scheduler or storage service is implied.

**Tech Stack:** C++20 GuestMemory, existing Clang/CMake diagnostic, CTest and Python unittest. Continue the authorized development branch and retained generated source.

- [x] Write `tests/hardware_info_test.cpp` before implementing `src/hardware_info.h/.cpp`. API: `class HardwareInfo { public: static constexpr uint32_t address=0x71200000; explicit HardwareInfo(GuestMemory&); };`. Tests must read `00 00 00 20` and confirm mask0x10 clear, reject byte+4 and word+1/cross-prefix reads/writes without modifications, reject preceding byte, and reject preexisting/duplicate mappings without overwriting sentinels. Run with an empty constructor first to prove tests fail on missing mapping.
- [x] Implement constructor with `memory.map(address,4); memory.store<uint32_t>(address,0x20);`. Add `sfr_hardware_info_test` to CMake/PowerShell build targets and source to the diagnostic executable. Validate with `ctest --test-dir out/build/host --output-on-failure` after building.
- [x] Add actual-entry regression asserting absence of `STOP import-variable @0x82000798`, observe RED, then initialize HardwareInfo after input verification. Bind only the exact name/address pair in the existing variable loop with `memory.store<uint32_t>(value, sfr::HardwareInfo::address)`. Log the binding/profile so the integration trace is reviewable. All other unsupported variable guards stay intact. Build with `./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-vm` and run `./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader`; assert the next exact STOP and LR/call count after observing them.
- [x] Run complete unittest suite with SFR_IMAGE_DUMP, SFR_CPU_DIAGNOSTIC, SFR_IMAGE_DIRECTORY and SFR_SOURCE_XEX set as in README, all CTest targets, and `python scripts/bootstrap.py --verify-only`. Obtain independent spec then quality review; update README/progress/toolchain/contract and pinned source attribution. Commit source/config/docs only.

Evidence: Xenia revision95a5c3ee250f80c3b9d139658649d9ffb6db3eec, `src/xenia/kernel/xboxkrnl/xboxkrnl_module.cc` lines136–153: 16-byte allocation, flags0x20, CPU-count6 and uncertainty over remaining11bytes. Both locally pinned Marathon/Unleashed imports.cpp only log a stub. Free Riders `sub_824DF608` loads the import pointer, loads offset0, masks0x10 and branches. Keep the unprovided suffix inaccessible instead of inventing zero-filled hardware data.
