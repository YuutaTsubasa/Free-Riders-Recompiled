#!/usr/bin/env bash
# Builds the tools, tests and (with --diagnostic DIR) the game on Linux with
# clang, SDL2 and Vulkan. Run python3 scripts/bootstrap.py first.
# Usage: scripts/build_linux.sh [--diagnostic out/recomp/diagnostic] [--build DIR] [targets...]
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="${SFR_LINUX_BUILD:-$HOME/sfr-build}"
diagnostic=""
targets=()
while [ $# -gt 0 ]; do
    case "$1" in
        --diagnostic) diagnostic="$(cd "$root" && realpath "$2")"; shift 2 ;;
        --build) build="$2"; shift 2 ;;
        *) targets+=("$1"); shift ;;
    esac
done
cc="${CC:-clang-15}"
cxx="${CXX:-clang++-15}"
options=(-S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release
         -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" -DCMAKE_POLICY_VERSION_MINIMUM=3.5)
if [ -n "$diagnostic" ]; then
    options+=(-DSFR_BUILD_DIAGNOSTIC=ON -DSFR_DIAGNOSTIC_DIR="$diagnostic")
fi
cmake "${options[@]}"
cmake --build "$build" ${targets:+--target "${targets[@]}"}
