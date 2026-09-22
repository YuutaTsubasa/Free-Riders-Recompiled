# Android ARM64 core build verification

**Goal:** Cross-compile and link the existing runtime test executables for Android ARM64 using the installed NDK, without claiming device execution or an APK.

**Environment:** Android NDK `29.0.13599879-beta2` (r29-beta2), Windows x86-64 host, aarch64-linux-android26 target. adb reports no attached devices. Outputs stay in ignored `out/build/android-arm64`.

- [x] Compile the ten runtime source files with the NDK C++20 compiler and link all ten existing native test executables, using static libc++ for standalone test deployment.
- [x] Check produced ELF headers/program headers with the NDK readelf tool: AArch64, Android interpreter, PIE and load-segment alignment; record compiler/version and exit results.
- [x] If compilation fails, diagnose and fix only established portability issues with focused regressions and independent review. Do not weaken runtime memory checks or assume 16 KiB-page execution works.
- [x] Record build-only status, absent device execution, no APK/input integration and remaining page-size verification in progress/toolchain docs; commit verified source/docs only.
