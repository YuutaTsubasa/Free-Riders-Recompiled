# Android ARM64 core build verification

The ten existing runtime test executables compile and link for `aarch64-unknown-linux-android26` with the locally installed Android NDK `29.0.13599879-beta2` (r29-beta2). Its compiler reports Android build 13065274, Clang 20.0.0, LLVM revision `b718bcaf8c198c82f3021447d943401e3ab5bd54`. This uses the installed toolchain and is not a recommendation to select a beta NDK for a release.

All ten artifacts were inspected: ELF64 little-endian, AArch64 machine type, PIE/DYN, interpreter `/system/bin/linker64`, and each LOAD segment aligned to 16384 bytes with congruent file/virtual offsets. The vector test was also inspected with NDK llvm-readelf. Static libc++ was selected for these standalone probes; the Android system libraries/interpreter are still required.

No Android device was attached according to adb. The binaries have not been executed on Android and are not an APK, JNI integration, input backend or full generated CPU diagnostic. API 26 is the probe target, not an established minimum version for the eventual game. ELF 16 KiB alignment does not prove runtime page-size compatibility: GuestMemory currently requires mapping addresses aligned to the host page size, while the tested guest allocator and some fixtures assume 4 KiB granularity. Both 4 KiB and 16 KiB Android execution need separate verification and possibly mapping changes.

The exact build sequence from the repository root in PowerShell is:

```powershell
$taskNdkBin = Join-Path $env:LOCALAPPDATA 'Android/Sdk/ndk/29.0.13599879/toolchains/llvm/prebuilt/windows-x86_64/bin'
$taskCompiler = Join-Path $taskNdkBin 'aarch64-linux-android26-clang++.cmd'
$taskBuild = 'out/build/android-arm64'
New-Item -ItemType Directory -Force -Path $taskBuild | Out-Null
$taskUnits = @('guest_memory', 'xex_module', 'virtual_memory', 'critical_section', 'hardware_info', 'thread_local_storage', 'system_time', 'guest_clock', 'timestamp_bundle', 'vector_memory')
$taskObjects = @()
foreach ($taskUnit in $taskUnits) {
    $taskObject = "$taskBuild/$taskUnit.o"
    & $taskCompiler -std=c++20 -O2 -Isrc -c "src/$taskUnit.cpp" -o $taskObject
    if ($LASTEXITCODE -ne 0) { throw "Compile failed: $taskUnit" }
    $taskObjects += $taskObject
}
foreach ($taskUnit in $taskUnits) {
    & $taskCompiler -std=c++20 -O2 -Isrc "tests/${taskUnit}_test.cpp" @taskObjects -static-libstdc++ -o "$taskBuild/${taskUnit}_test"
    if ($LASTEXITCODE -ne 0) { throw "Link failed: $taskUnit" }
}
& (Join-Path $taskNdkBin 'llvm-readelf.exe') -h -l "$taskBuild/vector_memory_test"
```

No production code change was required. Ignored evidence: `out/android-core-build.log`, `out/android-core-elf-check.log` and `out/build/android-arm64`. Windows still provides the actual game-entry diagnostic run; Linux/WSL2 provides executed core tests; Android currently provides build verification only. macOS/iOS remain unverified.
