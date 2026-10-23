"""Copy generated diagnostic code keeping label checkpoints only at loop heads.

The generated code calls sfr::guest_checkpoint(ctx) after every label (172,524
in the shipped output). A label that no goto at or after it in the same
function jumps to is reached at most once between two function entries, so its
checkpoint only counts down towards the next permit call; the function's entry
(sfr::enter_function) and its loop heads still checkpoint, so every path that
repeats still passes one. Guest-visible behaviour is unchanged: a checkpoint
reads and writes no guest state. What changes is how much guest work the
countdown spans (docs/performance.md, 2026-10-20: generated code).

This works on an existing output directory, without regenerating it (the
shipped generated code must not be regenerated with a newer generator).

usage: python scripts/loop_head_checkpoints.py <generated dir> <output dir>
       python scripts/loop_head_checkpoints.py --check <generated dir> <output dir>
       python scripts/loop_head_checkpoints.py --file <ppc_recomp.N.cpp> <output file>
The output holds every file of the input; only the ppc_recomp.*.cpp files lose
checkpoint lines, and --check verifies that the output is exactly the input
less those lines. --file converts one source (the build does this for each,
CMake option SFR_LOOP_HEAD_CHECKPOINTS); output that has only loop-head
checkpoints already comes out unchanged.
"""
import re
import shutil
import sys
from pathlib import Path

CHECKPOINT = '\tsfr::guest_checkpoint(ctx);'
LABEL = re.compile(r'(loc_[0-9A-Fa-f]+):\r?$')
GOTO = re.compile(r'goto (loc_[0-9A-Fa-f]+);')
GENERATED = re.compile(r'ppc_recomp\.\d+\.cpp')


def forward_checkpoints(lines):
    """Indices of checkpoint lines that follow a label no goto returns to."""
    drop = []
    starts = [i for i, line in enumerate(lines) if line.startswith('PPC_FUNC_IMPL(')] + [len(lines)]
    for begin, end in zip(starts, starts[1:]):
        labels, loops = {}, set()
        for i in range(begin, end):
            line = lines[i]
            match = LABEL.match(line)
            if match:
                labels[match[1]] = i
            for target in GOTO.findall(line):
                if target in labels:  # defined at or above this goto: it can come back
                    loops.add(target)
        for name, i in labels.items():
            if name not in loops and i + 1 < end and lines[i + 1].rstrip('\r') == CHECKPOINT:
                drop.append(i + 1)
    return drop


def transform(text):
    lines = text.split('\n')
    drop = set(forward_checkpoints(lines))
    kept = sum(1 for line in lines if line.rstrip('\r') == CHECKPOINT) - len(drop)
    return '\n'.join(line for i, line in enumerate(lines) if i not in drop), len(drop), kept


def check(source, output):
    """The output is the source less checkpoint lines, in order; nothing else."""
    a, b = source.split('\n'), output.split('\n')
    j = 0
    for line in a:
        if j < len(b) and b[j] == line:
            j += 1
        elif line.rstrip('\r') != CHECKPOINT:
            return False
    return j == len(b)


def convert_file(source, output):
    result, _, _ = transform(source.read_bytes().decode('utf-8'))
    data = result.encode('utf-8')
    output.parent.mkdir(parents=True, exist_ok=True)
    # Written in full every time, so the build sees it newer than its source.
    output.write_bytes(data)


def main(argv):
    if argv[:1] == ['--file']:
        if len(argv) != 3:
            sys.exit(__doc__)
        source, output = Path(argv[1]), Path(argv[2])
        if source.resolve() == output.resolve():
            sys.exit('the output must be another file')
        convert_file(source, output)
        return
    verify = argv[:1] == ['--check']
    if verify:
        argv = argv[1:]
    if len(argv) != 2:
        sys.exit(__doc__)
    source, output = Path(argv[0]), Path(argv[1])
    if source.resolve() == output.resolve():
        sys.exit('the output must be another directory')
    output.mkdir(parents=True, exist_ok=True)
    removed = kept = 0
    for path in sorted(source.iterdir()):
        if not path.is_file():
            continue
        target = output / path.name
        if not GENERATED.fullmatch(path.name):
            if verify:
                if target.read_bytes() != path.read_bytes():
                    sys.exit(f'{path.name} differs')
            else:
                shutil.copyfile(path, target)
            continue
        text = path.read_bytes().decode('utf-8')
        result, dropped, left = transform(text)
        removed += dropped
        kept += left
        if verify:
            written = target.read_bytes().decode('utf-8')
            if written != result or not check(text, written):
                sys.exit(f'{path.name} is not the input less forward checkpoints')
        else:
            target.write_bytes(result.encode('utf-8'))
    print(f'label checkpoints kept {kept} (loop heads), removed {removed}')


if __name__ == '__main__':
    main(sys.argv[1:])
