#!/usr/bin/env python3
"""Sums up the race frames of benchmark runs (scripts/benchmark.ps1).

Each run is a game log, stderr of sfr_cpu_diagnostic, with one NATIVE_PRESENT
line a frame. Only the frames of the race count (racing=1), less the first
--skip of them (the countdown, and shaders and pipelines made on first use).
A frame's time is the gap between its present and the one before.

    python scripts/benchmark_summary.py out/bench/<run> [--skip 600]

prints a table per configuration and writes it to <run>/summary.md.
"""
import argparse
import re
import statistics
from pathlib import Path

PRESENT = re.compile(r'^NATIVE_PRESENT .*\bframe=(\d+)\b')
FIELD = re.compile(r'\b([a-z_0-9]+)=([^\s]+)')
# The per-frame costs worth comparing, averaged over the measured frames.
AVERAGED = ('draw_ms', 'present_ms', 'gpu_wait_ms', 'main_queued_ms', 'main_blocked_ms', 'main_ready_ms')


def holder_ms(text, guest):
    """What 'holders=1:12.30,7:3.10' says the guest thread held, in ms."""
    for item in text.split(','):
        who, _, ms = item.partition(':')
        if who == str(guest):
            return float(ms)
    return 0.0


def read_run(path, skip):
    """The measured race frames of one log, and how the run ended."""
    frames, racing_seen, stop = [], 0, None
    previous = None
    with open(path, encoding='utf-8', errors='replace') as log:
        for line in log:
            if line.startswith('STOP '):
                stop = line.strip()
                continue
            if line.startswith('HANG_REPORT') and not stop:
                stop = line.strip()
                continue
            if not PRESENT.match(line):
                continue
            fields = dict(FIELD.findall(line))
            seconds = float(fields.get('seconds', 'nan'))
            gap_ms = (seconds - previous) * 1000 if previous is not None else None
            previous = seconds
            if fields.get('racing') != '1':
                continue
            racing_seen += 1
            if racing_seen <= skip or gap_ms is None:
                continue
            frame = {'ms': gap_ms, 'draws': int(fields.get('draws', 0)),
                     'pipelines': int(fields.get('pipelines', 0)), 'pipeline_ms': float(fields.get('pipeline_ms', 0))}
            for name in AVERAGED:
                frame[name] = float(fields.get(name, 0))
            frame['main_held_ms'] = holder_ms(fields.get('holders', ''), 1)
            frames.append(frame)
    ended = 'present-limit' if stop and 'present-limit' in stop else (stop or 'no STOP line (killed or crashed)')
    return frames, racing_seen, ended


REASON = re.compile(r'^PARALLEL_REASON reason=(0x[0-9a-f]+) kind=(\w+)(?: name=(\S+))?')
KINDS = {1: 'import', 2: 'hook', 3: 'memory'}


def read_reasons(path, skip=0):
    """Why the main thread queued during the race (SFR_PARALLEL_HELD=1):
    {(guest, reason): ms} summed over the measured race frames, the names of
    the reasons, and how many frames were summed."""
    queued, names, racing_seen, frames = {}, {}, 0, 0
    with open(path, encoding='utf-8', errors='replace') as log:
        for line in log:
            match = REASON.match(line)
            if match:
                names[int(match.group(1), 16)] = (match.group(2), match.group(3) or '')
                continue
            if not line.startswith('NATIVE_PRESENT') or ' racing=1 ' not in line:
                continue
            racing_seen += 1
            if racing_seen <= skip:
                continue
            frames += 1
            fields = dict(FIELD.findall(line))
            for item in filter(None, fields.get('main_blockers_by_reason', '').split(',')):
                guest, reason, ms = item.split(':')
                key = (int(guest), int(reason, 16))
                queued[key] = queued.get(key, 0.0) + float(ms)
    return queued, names, frames


def held_table(directory, skip=0, top=15):
    """The reasons the main thread queued behind longest, per race frame,
    over every run that reported them."""
    total, names, frames = {}, {}, 0
    for log in sorted(Path(directory).glob('*.log')):
        queued, run_names, run_frames = read_reasons(log, skip)
        if not queued:
            continue
        names.update(run_names)
        frames += run_frames
        for key, ms in queued.items():
            total[key] = total.get(key, 0.0) + ms
    if not total or not frames:
        return ''
    lines = ['主執行緒排隊時，擋住它的執行緒在做什麼（比賽中，每格平均）：', '',
             '| 客體 | 種類 | 位址 | 名稱 | 每格 ms |', '| ---: | --- | --- | --- | ---: |']
    for (guest, reason), ms in sorted(total.items(), key=lambda item: -item[1])[:top]:
        if reason == 0:
            kind, address, name = '自己的程式', '', ''
        else:
            kind, name = names.get(reason, (KINDS.get(reason >> 32, '?'), ''))
            address = f'0x{reason & 0xFFFFFFFF:08x}'
        lines.append(f'| {guest} | {kind} | {address} | {name} | {ms / frames:.3f} |')
    return '\n'.join(lines)


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(fraction * len(ordered)))]


def describe(frames):
    times = [f['ms'] for f in frames]
    result = {
        'frames': len(frames),
        'median_ms': statistics.median(times),
        'p95_ms': percentile(times, 0.95),
        'p99_ms': percentile(times, 0.99),
        'fps': 1000 * len(times) / sum(times),
        'draws': statistics.median(f['draws'] for f in frames),
    }
    for name in AVERAGED + ('main_held_ms',):
        result[name] = statistics.fmean(f[name] for f in frames)
    # What a player feels is the long frames, not the mean: how many there are,
    # and how many of them are a pipeline made on the spot (docs/roadmap.md: 0 to 2 a race).
    result['slow50'] = sum(1 for t in times if t > 50)
    result['slow80'] = sum(1 for t in times if t > 80)
    result['hitch30'] = sum(1 for f in frames if f['pipeline_ms'] >= 5 and f['ms'] > 30)
    result['pipelines'] = sum(f['pipelines'] for f in frames)
    return result


COLUMNS = (('fps', '平均 fps', '{:.1f}'), ('median_ms', '中位數 ms', '{:.1f}'), ('p95_ms', 'P95 ms', '{:.1f}'),
           ('p99_ms', 'P99 ms', '{:.1f}'), ('main_held_ms', '主執行緒持有 ms', '{:.1f}'),
           ('main_queued_ms', '主執行緒排隊 ms', '{:.1f}'), ('draw_ms', '繪製 ms', '{:.1f}'),
           ('present_ms', 'present ms', '{:.1f}'), ('slow50', '>50 ms 格', '{}'), ('hitch30', '編譯卡頓格', '{}'),
           ('pipelines', '現場建 pipeline', '{}'), ('frames', '量到的格數', '{}'))


def summarise(directory, skip):
    """(markdown table, rows) for every <config>-<n>.log in directory."""
    runs = {}
    for log in sorted(Path(directory).glob('*.log')):
        config, _, repeat = log.stem.rpartition('-')
        if not config or not repeat.isdigit():
            continue
        runs.setdefault(config, []).append((int(repeat), log))
    lines = ['| 設定 | 趟 | ' + ' | '.join(title for _, title, _ in COLUMNS) + ' | 結束 |',
             '| --- | --- |' + ' ---: |' * len(COLUMNS) + ' --- |']
    rows = []
    # The warm-up first, then the settings in the order they ran.
    for config, logs in sorted(runs.items(), key=lambda item: (item[0] != 'warmup', min(p.stat().st_mtime for _, p in item[1]))):
        for repeat, log in sorted(logs):
            frames, seen, ended = read_run(log, skip)
            if not frames:
                note = f'沒有比賽畫面（racing 格數 {seen}）；{ended}'
                lines.append(f'| {config} | {repeat} | ' + ' | '.join('-' for _ in COLUMNS) + f' | {note} |')
                rows.append((config, repeat, None, ended))
                continue
            stats = describe(frames)
            rows.append((config, repeat, stats, ended))
            lines.append(f'| {config} | {repeat} | ' +
                         ' | '.join(fmt.format(stats[key]) for key, _, fmt in COLUMNS) + f' | {ended} |')
    lines += ['', compare(rows)]
    held = held_table(directory, skip)
    if held:
        lines += ['', held]
    return '\n'.join(lines), rows


MINIMUM_ROUNDS = 4  # fewer pairs than this prove nothing, whatever the ratios
EXPECTED_FRAMES_TOLERANCE = 0.02  # a run with fewer race frames than the others did not play the same race


def valid_runs(rows):
    """(config, repeat, stats) of the runs that can be compared, and why the others cannot."""
    counts = [stats['frames'] for _, _, stats, _ in rows if stats]
    expected = statistics.median(counts) if counts else 0
    good, problems = [], []
    for config, repeat, stats, ended in rows:
        if config == 'warmup':
            continue
        if not stats:
            problems.append(f'{config}-{repeat}: 沒有走進比賽，沒有可量的比賽畫面（{ended}）')
        elif 'present-limit' not in ended:
            problems.append(f'{config}-{repeat}: 沒有跑到 present-limit（{ended}）')
        elif abs(stats['frames'] - expected) > EXPECTED_FRAMES_TOLERANCE * expected:
            problems.append(f"{config}-{repeat}: 比賽格數 {stats['frames']}，其他趟約 {expected:.0f}")
        else:
            good.append((config, repeat, stats))
    return good, problems


def compare(rows):
    """Each setting against baseline: the median run, then round by round.

    The settings take turns (benchmark.ps1), so run n of each belongs to the same
    stretch of the PC's day. The paired ratios are what says whether a setting
    differs: only a change that is the same way in most rounds and larger than
    the baseline's own spread is called one.
    """
    good, problems = valid_runs(rows)
    by_config = {}
    for config, repeat, stats in good:
        by_config.setdefault(config, {})[repeat] = stats
    if not by_config:
        return '沒有任何一趟可以比較。\n' + '\n'.join(f'- {p}' for p in problems)
    lines = []
    if problems:
        lines += ['**以下幾趟不納入比較：**'] + [f'- {p}' for p in problems] + ['']
    median = {config: {key: statistics.median(s[key] for s in runs.values()) for key in ('fps', 'median_ms', 'p95_ms', 'slow50')}
              for config, runs in by_config.items()}
    reference = median.get('baseline')
    lines += ['| 設定 | 趟數 | 平均 fps | 中位數 ms | P95 ms | >50 ms 格 | 相對 baseline |', '| --- | ---: | ---: | ---: | ---: | ---: | ---: |']
    for config, value in median.items():
        change = f"{(value['fps'] / reference['fps'] - 1) * 100:+.0f}%" if reference else '-'
        lines.append(f"| {config} | {len(by_config[config])} | {value['fps']:.1f} | {value['median_ms']:.1f} | "
                     f"{value['p95_ms']:.1f} | {value['slow50']:.0f} | {change} |")
    baseline = by_config.get('baseline', {})
    if len(baseline) >= 2:
        fps = [s['fps'] for s in baseline.values()]
        spread = (max(fps) - min(fps)) / statistics.median(fps)
        lines += ['', f'baseline 自己各趟之間的差距（最大減最小，除以中位數）：**{spread * 100:.0f}%**'
                      f'（{min(fps):.1f}–{max(fps):.1f} fps）。其他設定的差距必須明顯大於這個數字才算數。']
        lines += ['', '逐輪成對比較（同一輪的該設定 fps 除以 baseline 的 fps）：', '',
                  '| 設定 | 每輪比值 | 中位數 | 較快的輪數 | 判定 |', '| --- | --- | ---: | ---: | --- |']
        for config, runs in by_config.items():
            if config == 'baseline':
                continue
            rounds = sorted(set(runs) & set(baseline))
            if len(rounds) < 2:
                continue
            ratios = [runs[r]['fps'] / baseline[r]['fps'] for r in rounds]
            middle = statistics.median(ratios)
            faster = sum(1 for r in ratios if r > 1)
            consistent = max(faster, len(ratios) - faster) / len(ratios) >= 0.8
            threshold = max(0.05, spread / 2)
            if len(ratios) < MINIMUM_ROUNDS:
                verdict = f'輪數不足（至少 {MINIMUM_ROUNDS} 輪才下判定）'
            elif abs(middle - 1) > threshold and consistent:
                verdict = '較快' if middle > 1 else '較慢'
            else:
                verdict = '不能判定（在雜訊內）'
            lines.append(f"| {config} | {' '.join(f'{r:.2f}' for r in ratios)} | {middle:.2f} | {faster}/{len(ratios)} | {verdict} |")
    else:
        lines += ['', 'baseline 不到兩趟，無法估計雜訊，也無法逐輪比較。']
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('directory')
    parser.add_argument('--skip', type=int, default=600, help='race frames left out at the start (default 600)')
    args = parser.parse_args()
    table, _ = summarise(args.directory, args.skip)
    print(table)
    (Path(args.directory) / 'summary.md').write_text(table + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
