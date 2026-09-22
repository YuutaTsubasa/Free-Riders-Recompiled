#!/usr/bin/env bash
# Starts Sonic Free Riders in the native runtime on Linux, as scripts/play.ps1
# does on Windows: no diagnostic time or call limits. Build first with
# scripts/build_linux.sh --diagnostic out/recomp/diagnostic.
# Options: --skip-movies, --race-render-every N, --serial, --no-vertex-cache,
# --mute, --region NAME, --log FILE (docs/linux.md).
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
exe="${SFR_LINUX_BUILD:-$HOME/sfr-build}/sfr_cpu_diagnostic"
skip_movies="" render_every=1 parallel=cores vertex_cache=1 audio=1 region=ntsc-us log=out/play.log
while [ $# -gt 0 ]; do
    case "$1" in
        --skip-movies) skip_movies=1; shift ;;
        --race-render-every) render_every="$2"; shift 2 ;;
        --serial) parallel=0; shift ;;
        --no-vertex-cache) vertex_cache=0; shift ;;
        --mute) audio=0; shift ;;
        --region) region="$2"; shift 2 ;;
        --log) log="$2"; shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
[ -x "$exe" ] || { echo "Build first: scripts/build_linux.sh --diagnostic DIR (missing $exe)" >&2; exit 1; }
cd "$root"
for path in out/recomp/image-loader private/assets; do
    [ -e "$path" ] || { echo "Missing $path (see README)" >&2; exit 1; }
done
export SFR_CALL_BUDGET=18446744073709551615 SFR_WATCHDOG_SECONDS=31536000
export SFR_ALLOW_RENDER_TARGETS=1 SFR_TRACE_GRAPHICS=0 SFR_DIAGNOSTIC_ENTRIES=0 SFR_TRACE_IMPORTS=0
export SFR_RENDER_EVERY="$render_every" SFR_PARALLEL_WORKER="$parallel" SFR_FRAME_LIMIT=60
export SFR_VERTEX_CACHE="$vertex_cache" SFR_AUDIO="$audio" SFR_SKIP_MOVIES="$skip_movies"
# Signed in, so the game keeps records in save/ (docs/saves.md).
export SFR_PROFILE=1
# Precompiled shaders (scripts/pack_shaders.py) spare the runtime compiler.
[ -f out/shaders/shaders.pack ] && export SFR_SHADER_PACK="${SFR_SHADER_PACK:-out/shaders/shaders.pack}"
mkdir -p "$(dirname "$log")"
echo "Running; trace output goes to $log. Close the game window or press Ctrl+C here to stop."
status=0
"$exe" out/recomp/image-loader private/assets --game-region="$region" 2> "$log" || status=$?
[ "$status" = 0 ] || echo "Stopped (exit $status); the last lines of $log say why."
