# Linux runtime core verification

Historical: the complete game now builds and runs on Linux; see [linux.md](linux.md).

The ten existing native runtime test executables were compiled and run successfully on Arch Linux under WSL2, using GCC 14.2.1 (20240910). The kernel reported `Linux 6.18.33.2-microsoft-standard-WSL2 x86_64`; `getconf PAGESIZE` returned 4096. No source changes were needed for this run.

This exercises the POSIX mmap/mprotect implementation, big-endian scalar/vector memory, XEX metadata, virtual allocation, critical sections, hardware flags, TLS, system time and monotonic clock helpers. It does not compile or execute the complete generated PowerPC diagnostic, graphics, audio or input. It also does not verify a standalone Linux installation, a different page size, macOS, Android or iOS.

From the repository root in Linux, the exact standalone build/test loop is:

```sh
set -eu
build=out/build/linux-gcc
mkdir -p "$build"
objects=
for unit in guest_memory xex_module virtual_memory critical_section hardware_info thread_local_storage system_time guest_clock timestamp_bundle vector_memory; do
    g++ -std=c++20 -O2 -Isrc -c "src/$unit.cpp" -o "$build/$unit.o"
    objects="$objects $build/$unit.o"
done
for unit in guest_memory xex_module virtual_memory critical_section hardware_info thread_local_storage system_time guest_clock timestamp_bundle vector_memory; do
    g++ -std=c++20 -O2 -Isrc "tests/${unit}_test.cpp" $objects -o "$build/${unit}_test"
    "$build/${unit}_test"
done
```

All ten exited zero with their success messages. This used the existing GCC installation; no package upgrades or distribution configuration changes were made. Local evidence is in ignored `out/linux-core-verification.log` and `out/build/linux-gcc`. The root CMake diagnostic still requires Clang and its generated sources; that build remains a separate next step.
