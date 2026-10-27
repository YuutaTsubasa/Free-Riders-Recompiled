"""Compiler launcher for builds with a PGO profile (SFR_PGO, CMakeLists.txt).

    pgo_compile.py COMPILER ARGS...      run the compiler, pass its output through
    pgo_compile.py --summary BUILD_DIR   print the total after the game links

Clang reports each function whose code changed since the profile was
collected ("function control flow change detected (hash mismatch)") and
compiles it without the profile. The launcher keeps that count per object in
<object>.pgo-stale so the build can print one total, which says when the
profile should be collected again (data/pgo/README.md).
"""
import subprocess
import sys
from pathlib import Path

MARK = b"hash mismatch"


def summary(build):
    files = [(p, int(p.read_text() or 0)) for p in Path(build).rglob("*.pgo-stale")]
    stale = sum(n for _, n in files)
    if stale:
        print(f"warning: SFR_PGO: {stale} function(s) in {len(files)} file(s) changed since the profile "
              "was collected and were compiled without it; collect the profile again (data/pgo/README.md)",
              flush=True)


def compile(args):
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    sys.stdout.buffer.write(result.stdout)
    sys.stdout.buffer.flush()
    sys.stderr.buffer.write(result.stderr)
    sys.stderr.buffer.flush()
    if "-o" in args[:-1]:
        marker = Path(args[args.index("-o") + 1] + ".pgo-stale")
        stale = sum(line.count(MARK) for line in result.stderr.splitlines())
        if stale and result.returncode == 0:
            marker.write_text(f"{stale}\n")
        else:
            marker.unlink(missing_ok=True)
    return result.returncode


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--summary":
        summary(sys.argv[2])
    elif len(sys.argv) > 1:
        sys.exit(compile(sys.argv[1:]))
    else:
        sys.exit(__doc__)
