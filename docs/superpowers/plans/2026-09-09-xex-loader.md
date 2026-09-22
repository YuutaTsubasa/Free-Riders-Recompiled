# XEX module metadata implementation plan

> **For agentic workers:** Use superpowers:subagent-driven-development for the bounded loader component and reviews; the primary agent integrates the native diagnostic.

**Goal:** Advance the real native entry past the executable-module variable and header lookup, retaining an explicit stop at the next unsupported service.

**Architecture:** Preserve the verified original XEX header in the image dump. A small guest-memory module exposes the two-level handle and the observed loader field at offset 0x58, plus bounded optional-header lookup. Only the verified RtlImageXexHeaderField name/address pair is dispatched to this implementation; all other imports still stop.

**Tech Stack:** C++20, Python unittest, Clang/CMake, existing XenonRecomp diagnostic output.

## Tasks

- [x] Add synthetic native tests in `tests/xex_module_test.cpp` for double indirection, immediate/inline/offset/variable header entries, absent key, malformed ranges and invalid header pointers. Observe failure before implementing `src/xex_module.{h,cpp}`. Unprovided loader fields must stop rather than return fabricated zero data.
- [x] Add native integration regressions in `tests/test_native_boot.py`: actual entry passes the old stop and logs header key 0x20401 returning zero (absent in this source); missing or modified header is rejected before ENTER. Add a verified-source dump regression in `tests/test_image_dump.py` for the exact 0x5000-byte header. Run against the existing binaries to observe missing behavior.
- [x] Export `xex_header.bin` in `src/image_dump.cpp` before the completion marker. In `src/diagnostic_main.cpp`, validate size and SHA-1 `82ef42d3f130b26563fcd85909f2eab613de6c68` before constructing the module. Bind only variable name `__imp__XexExecutableModuleHandle` at 0x82000778, leaving all others trapped.
- [x] Replace the generated import-stop entry with `dispatch_import` in `scripts/generate_diagnostic.py`, its test and `src/diagnostic_hooks.h`. The native dispatcher handles only `__imp__RtlImageXexHeaderField` at 0x82ACB67C using guest r3/r4 and returns the guest result in r3; log the call. Other name/address pairs throw the existing import-function stop.
- [x] Add the loader source and test target to `CMakeLists.txt` and the build script. Build the tools, produce a new image dump and regenerate diagnostic sources into new ignored directories. Build and run the native executable with the new dump; record the first new stop without asserting gameplay success.
- [x] Run all Python integration tests with the native executables and verified source paths configured, and all CTest targets. Complete independent specification and quality reviews, fix findings, update README/progress/toolchain and commit project code only.

## Evidence and scope

The actual sub_824DCF70 reads import slot 0x82000778, dereferences its value, loads module+0x58, and calls RtlImageXexHeaderField with key 0x20401. The supplied header has 16 optional entries and no such key. This is a metadata implementation, not a dynamic loader, scheduler, heap, or playable game. The existing development branch is continued in place to reuse the verified private assets and generated build; no concurrent user edits were present.
