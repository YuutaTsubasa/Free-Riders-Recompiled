#!/usr/bin/env bash
# Builds the game for Android (libmain.so and libSDL2.so per ABI) with the
# NDK, then packages the APK (scripts/package_android.py, offline). Run python
# scripts/bootstrap.py first and generate the game code (README).
# Usage: scripts/build_android.sh [--diagnostic DIR] [--abi x86_64|arm64-v8a]... [--no-apk] [--launcher-only]
# --launcher-only builds the launcher and SDL without the game (no generated
# code needed, no APK; what the CI does).
# Environment: ANDROID_HOME (default ~/AppData/Local/Android/Sdk on Windows,
# ~/Android/Sdk elsewhere), SFR_ANDROID_NDK (default: newest NDK there).
set -euo pipefail
# Native paths for a Windows CMake under Git Bash/MSYS.
native() { if command -v cygpath >/dev/null; then cygpath -m "$1"; else echo "$1"; fi; }
root="$(native "$(cd "$(dirname "$0")/.." && pwd)")"
diagnostic="out/recomp/diagnostic"
abis=()
apk=1
game=1
while [ $# -gt 0 ]; do
    case "$1" in
        --diagnostic) diagnostic="$2"; shift 2 ;;
        --abi) abis+=("$2"); shift 2 ;;
        --no-apk) apk=0; shift ;;
        --launcher-only) game=0; apk=0; shift ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
[ ${#abis[@]} -gt 0 ] || abis=(arm64-v8a x86_64)
if [ -z "${ANDROID_HOME:-}" ]; then
    if [ -d "$HOME/AppData/Local/Android/Sdk" ]; then ANDROID_HOME="$HOME/AppData/Local/Android/Sdk"; else ANDROID_HOME="$HOME/Android/Sdk"; fi
fi
ndk="$(native "${SFR_ANDROID_NDK:-$(ls -d "$ANDROID_HOME"/ndk/* | sort -V | tail -1)}")"
[ -f "$ndk/build/cmake/android.toolchain.cmake" ] || { echo "No Android NDK under $ANDROID_HOME/ndk" >&2; exit 1; }
if [ "$game" = 1 ]; then
    diagnostic="$(native "$(cd "$root" && cd "$diagnostic" && pwd)")"
    game_options=(-DSFR_BUILD_DIAGNOSTIC=ON -DSFR_DIAGNOSTIC_DIR="$diagnostic")
    targets=(sfr_cpu_diagnostic sfr_launcher SDL2)
    libraries=(libmain.so liblauncher.so tools/SDL/libSDL2.so)
else
    game_options=(-DSFR_BUILD_DIAGNOSTIC=OFF)
    targets=(sfr_launcher SDL2)
    libraries=(liblauncher.so tools/SDL/libSDL2.so)
fi
jni="$root/android/app/src/main/jniLibs"
for abi in "${abis[@]}"; do
    build="$root/out/build/android-$abi"
    cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$abi" -DANDROID_PLATFORM=android-28 -DANDROID_STL=c++_shared \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_TESTING=OFF "${game_options[@]}"
    cmake --build "$build" --target "${targets[@]}"
    mkdir -p "$jni/$abi"
    # Debug information stays in the build directory (the game's is ~800 MB).
    strip="$(ls -d "$ndk"/toolchains/llvm/prebuilt/*/bin/llvm-strip* | head -1)"
    for library in "${libraries[@]}"; do
        "$strip" --strip-debug -o "$jni/$abi/$(basename "$library")" "$build/$library"
    done
    triple=$([ "$abi" = arm64-v8a ] && echo aarch64-linux-android || echo x86_64-linux-android)
    cp "$(ls -d "$ndk"/toolchains/llvm/prebuilt/*/sysroot/usr/lib/$triple/libc++_shared.so | head -1)" "$jni/$abi/"
done
if [ "$apk" = 1 ]; then
    ANDROID_HOME="$ANDROID_HOME" python "$root/scripts/package_android.py"
fi
