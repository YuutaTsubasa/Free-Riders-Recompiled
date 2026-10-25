"""Compare the race state builds print with SFR_RACE_DIGEST.

Each game.log holds, every n-th race step, RACE_STATE (the race clock) and
RACE_RACER lines (each racer's first 3 KiB as words). Two runs of one build
do not run the same race all the way: a run takes one of a few courses from
the start (thread timing; with SFR_TEST_RAND_SEED, SFR_PARALLEL_WORKER=0 and
SFR_REALTIME_UI=0, a handful), and a run that took the same one as another
agrees with it word for word for a long stretch. So:

- noise: the words that differ between any two runs at the first sample with
  every racer (heap addresses, per-racer values set from the clock when the
  racers are made); they are left out everywhere;
- the horizon of two runs: the first sample after that where any other word
  differs (none: they agree to the end);
- each run's best match: the other baseline run with the longest horizon.

A candidate passes if its best match among the baseline runs reaches at least
as far as the shortest best match among the baseline runs themselves: it then
ran one of the races the baseline runs, as exactly as the baseline repeats it.

usage: python scripts/race_digest_compare.py --baseline A.log B.log [...] --candidate X.log [...]
"""
import argparse
import re
import sys
from pathlib import Path

STATE = re.compile(r'^RACE_STATE step=(\d+) racers=(\d+) clock=([0-9a-f,]*)')
RACER = re.compile(r'^RACE_RACER step=(\d+) racer=(\d+) words=([0-9a-f,]*)')


def load(path):
    """{step: {('clock', 0) or ('racer', i): [words]}}, plus the racer count per step."""
    steps = {}
    with open(path, encoding='utf-8', errors='replace') as log:
        for line in log:
            match = STATE.match(line)
            if match:
                sample = steps.setdefault(int(match[1]), {})
                sample[('clock', 0)] = [int(w, 16) for w in match[3].split(',') if w]
                sample[('racers', 0)] = [int(match[2])]
                continue
            match = RACER.match(line)
            if match:
                steps.setdefault(int(match[1]), {})[('racer', int(match[2]))] = [
                    int(w, 16) for w in match[3].split(',') if w]
    return steps


def differences(a, b, noise=frozenset()):
    """(unit, word index) pairs that differ between two samples of one step."""
    found = set()
    for unit in set(a) | set(b):
        x, y = a.get(unit), b.get(unit)
        if x is None or y is None or len(x) != len(y):
            found.add((unit, -1))
            continue
        for i, (p, q) in enumerate(zip(x, y)):
            if p != q and (unit, i) not in noise:
                found.add((unit, i))
    return found


def describe(diff, limit=6):
    shown = sorted(diff, key=lambda d: (d[0], d[1]))[:limit]
    return ', '.join(f'{u[0]}{u[1]}+0x{4 * i:x}' if i >= 0 else f'{u[0]}{u[1]}(missing)' for u, i in shown)


def horizon(a, b, steps, noise):
    for step in steps:
        diff = differences(a[step], b[step], noise)
        if diff:
            return step, diff
    return None, set()


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--baseline', nargs='+', type=Path, required=True)
    parser.add_argument('--candidate', nargs='+', type=Path, default=[])
    args = parser.parse_args(argv)
    if len(args.baseline) < 2:
        parser.error('at least two baseline runs are needed')
    names = [f'{p.parent.name}/{p.name}' for p in args.baseline + args.candidate]
    runs = [load(p) for p in args.baseline + args.candidate]
    nb = len(args.baseline)
    common = sorted(set.intersection(*(set(r) for r in runs)))
    full = [s for s in common if all(r[s][('racers', 0)][0] > 1 for r in runs)]
    if not full:
        sys.exit('no common samples with every racer')
    first = full[0]
    noise = set()
    for i in range(len(runs)):
        for j in range(i + 1, len(runs)):
            noise |= differences(runs[i][first], runs[j][first])
    # Words that ever hold a heap address (0xE0000000 and up, aligned; as a
    # float that would be below -3.6e19) in any run.
    for run in runs:
        for step in full:
            for unit, values in run[step].items():
                for i, value in enumerate(values):
                    if value >= 0xE0000000 and not value & 3:
                        noise.add((unit, i))
    checked = [s for s in full if s > first]
    end = checked[-1] if checked else first
    words = sum(len(v) for v in runs[0][first].values())
    print(f'samples with every racer: steps {first}..{end} ({len(full)}), {words} words each, '
          f'{len(noise)} noise words')

    def reach(i, j):
        step, _ = horizon(runs[i], runs[j], checked, noise)
        return end + 1 if step is None else step

    def label(step):
        return 'to the end' if step > end else f'step {step}'

    best_base = []
    for i in range(nb):
        reaches = [(reach(i, j), j) for j in range(nb) if j != i]
        step, j = max(reaches)
        best_base.append(step)
        print(f'baseline {names[i]}: best match {names[j]}, agrees {label(step)}')
    bar = min(best_base)
    ok = True
    for k in range(nb, len(runs)):
        reaches = [(reach(k, j), j) for j in range(nb)]
        step, j = max(reaches)
        _, diff = horizon(runs[k], runs[j], checked, noise)
        passed = step >= bar
        ok &= passed
        print(f'candidate {names[k]}: best match {names[j]}, agrees {label(step)} '
              f'({"pass" if passed else "FAIL"}, bar {label(bar)})' +
              (f'; first difference: {describe(diff)}' if diff else ''))
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main(sys.argv[1:])
