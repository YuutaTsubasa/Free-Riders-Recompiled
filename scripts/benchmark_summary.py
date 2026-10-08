#!/usr/bin/env python3
"""Sums up the race frames of benchmark runs (scripts/benchmark.ps1).

Each run is a game log, stderr of sfr_cpu_diagnostic, with one NATIVE_PRESENT
line a frame. Only the frames of the race count (racing=1), less the first
--skip of them (the countdown, and shaders and pipelines made on first use).
A frame's time is the gap between its present and the one before.

    python scripts/benchmark_summary.py out/bench/<run> [--skip 600]

prints a table per configuration and writes it to <run>/summary.md.

The same number of frames is a shorter stretch of the race for a faster
setting, and the start of a race (the countdown, the first straight) is
lighter than what follows, so the frame-count table flatters whichever is
faster (+43% where the same seconds of the race gave +18%, on an i5-3470). A
second comparison counts the frames of one stretch of race time, the same for
every run: from --window-start seconds after the race began to the end of the
shortest run's race, at most --window-end.

Two more sections read the frame metrics as a report on this PC: where the
main thread's frame time goes, and, when the run has baseline, skip-draws and
render-50 (run_benchmark.bat report), what limits the frame rate here. The
summary opens with the PC itself: what info.txt recorded (benchmark.ps1 and
scripts/hardware_probe.ps1) and the adapter the game's own log says it drew
with, so summary.md is the whole of a performance report.
"""
import argparse
import re
import statistics
from pathlib import Path

PRESENT = re.compile(r'^NATIVE_PRESENT .*\bframe=(\d+)\b')
FIELD = re.compile(r'\b([a-z_0-9]+)=([^\s]+)')
# The per-frame costs worth comparing, averaged over the measured frames.
AVERAGED = ('draw_ms', 'present_ms', 'gpu_wait_ms', 'main_queued_ms', 'main_blocked_ms', 'main_ready_ms', 'pacing_ms')


def holder_ms(text, guest):
    """What 'holders=1:12.30,7:3.10' says the guest thread held, in ms."""
    for item in text.split(','):
        who, _, ms = item.partition(':')
        if who == str(guest):
            return float(ms)
    return 0.0


def read_run(path, skip, timeline=None, fixed_step=False, draws=None):
    """The measured race frames of one log, and how the run ended.

    timeline, when given, receives (seconds since the race began, frame ms)
    for every race frame, the skipped ones too. With fixed_step (benchmark.ps1
    -FixedStep: one original frame per present) the race's own time is the
    race frame's number over 60, the same in every run, not the wall clock.
    draws, when given, receives each timeline entry's draw count beside it."""
    frames, racing_seen, stop = [], 0, None
    previous = race_start = None
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
            if race_start is None:
                race_start = seconds
            if timeline is not None and gap_ms is not None:
                timeline.append(((racing_seen - 1) / 60 if fixed_step else seconds - race_start, gap_ms))
                if draws is not None:
                    draws.append(int(fields.get('draws', 0)))
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
    lines = ['What held the main thread up while it queued (in the race, mean per frame):', '',
             '| Guest | Kind | Address | Name | ms a frame |', '| ---: | --- | --- | --- | ---: |']
    for (guest, reason), ms in sorted(total.items(), key=lambda item: -item[1])[:top]:
        if reason == 0:
            kind, address, name = 'its own code', '', ''
        else:
            kind, name = names.get(reason, (KINDS.get(reason >> 32, '?'), ''))
            address = f'0x{reason & 0xFFFFFFFF:08x}'
        lines.append(f'| {guest} | {kind} | {address} | {name} | {ms / frames:.3f} |')
    return '\n'.join(lines)


def window_fps(timeline, start, end):
    """Frames per second over [start, end) seconds of the race."""
    times = [ms for t, ms in timeline if start <= t < end]
    return 1000 * len(times) / sum(times) if times else None


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


COLUMNS = (('fps', 'Mean fps', '{:.1f}'), ('median_ms', 'Median ms', '{:.1f}'), ('p95_ms', 'P95 ms', '{:.1f}'),
           ('p99_ms', 'P99 ms', '{:.1f}'), ('main_held_ms', 'Main held ms', '{:.1f}'),
           ('main_queued_ms', 'Main queued ms', '{:.1f}'), ('draw_ms', 'Draw ms', '{:.1f}'),
           ('present_ms', 'present ms', '{:.1f}'), ('slow50', 'Frames >50 ms', '{}'), ('hitch30', 'Compile hitches', '{}'),
           ('pipelines', 'Pipelines built', '{}'), ('frames', 'Frames measured', '{}'))


MINIMUM_WINDOW = 15.0  # seconds; a shorter common stretch says too little


def read_settings(directory):
    """The key=value words of info.txt's configs= line (benchmark.ps1), or {}."""
    info = Path(directory) / 'info.txt'
    if not info.is_file():
        return {}
    for line in info.read_text(encoding='utf-8-sig', errors='replace').splitlines():
        if line.startswith('configs='):
            return dict(FIELD.findall(line))
    return {}


def read_info(directory):
    """Every key=value line of info.txt (benchmark.ps1 and hardware_probe.ps1), or {}."""
    info = Path(directory) / 'info.txt'
    if not info.is_file():
        return {}
    pairs = {}
    for line in info.read_text(encoding='utf-8-sig', errors='replace').splitlines():
        key, _, value = line.partition('=')
        if value:
            pairs[key.strip()] = value.strip()
    return pairs


# What the game's own log says about the PC, written once at the start of a
# run: the backend and adapter it drew with (native_graphics.cpp), each D3D12
# adapter's answer when it tried them, and the fallback to Vulkan.
MACHINE_LINES = {'NATIVE_GRAPHICS ': 'graphics', 'NATIVE_GRAPHICS_FALLBACK ': 'fallback',
                 'NATIVE_GRAPHICS_D3D12_PROBE ': 'probes', 'NATIVE_RENDER_SCALE ': 'render_scale',
                 'NATIVE_HOST_PROCESSORS ': 'processors'}


def read_machine(directory, head=5000):
    """{'graphics': 'backend=... adapter=...', 'probes': [...], ...} from the first
    log of the run that has them, read no further than its first `head` lines."""
    logs = sorted(Path(directory).glob('*.log'), key=lambda p: (not p.name.startswith('baseline-'), p.name))
    for log in logs:
        found = {'probes': []}
        with open(log, encoding='utf-8', errors='replace') as text:
            for number, line in enumerate(text):
                if number >= head:
                    break
                for prefix, key in MACHINE_LINES.items():
                    if line.startswith(prefix):
                        if key == 'probes':
                            found['probes'].append(line[len(prefix):].strip())
                        else:
                            found[key] = line[len(prefix):].strip()
        if 'graphics' in found:
            return found
    return {}


def hardware_table(directory):
    """The PC, from info.txt and the game's own log: what a report needs first."""
    info, machine = read_info(directory), read_machine(directory)
    if not info and not machine:
        return ''
    gpus = [info[key] for key in sorted(info) if re.fullmatch(r'gpu\d+', key)]
    rows = [('Model', info.get('model')), ('CPU', ' / '.join(filter(None, (info.get('cpu'), info.get('cpu_cores'))))),
            ('Instruction sets', info.get('cpu_features')), ('Memory', info.get('memory')),
            ('GPU (as Windows sees it)', '; '.join(gpus) or info.get('driver') or info.get('gpu')),
            ('GPU the game used (its own log)', machine.get('graphics')), ('Rendering resolution', machine.get('render_scale')),
            ('Operating system', info.get('os_build') or info.get('os')),
            ('Power', ' / '.join(filter(None, (info.get('power'), info.get('power_mode'), info.get('chassis'))))),
            ('Windows settings', ' / '.join(f'{label}={info[key]}' for label, key in
                                        (('hardware GPU scheduling', 'gpu_scheduling'), ('Game Mode', 'game_mode'), ('VBS', 'vbs')) if info.get(key))),
            ('D3D12', info.get('d3d12_runtime')), ('Vulkan', info.get('vulkan_loader')),
            ('Version', ' / '.join(filter(None, (info.get('commit') and f"commit {info['commit']}", info.get('backend') and f"backend {info['backend']}")))),
            ('Main thread on', machine.get('processors'))]
    lines = ['### Hardware and drivers', '', '| Item | Value |', '| --- | --- |']
    lines += [f'| {label} | {value} |' for label, value in rows if value]
    notes = []
    # What the game itself found: read from its lines, not guessed.
    if machine.get('fallback'):
        failed = [p for p in machine['probes'] if 'usable=1' not in p]
        notes.append(f"**D3D12 could not be used; the game fell back to Vulkan** ({machine['fallback']})."
                     + (' What each adapter answered: ' + '; '.join(failed) if failed else ''))
    if any('command_list7=' in p for p in machine.get('probes', [])):
        notes.append('An adapter made a D3D12 device but no ID3D12GraphicsCommandList7: this PC\'s D3D12 runtime is too old'
                     ' (Windows 11 24H2 has it, or the Agility SDK in D3D12\\D3D12Core.dll beside the program).')
    if 'type=integrated' in machine.get('graphics', '') and len(gpus) > 1:
        notes.append('The game ran on the integrated GPU while this PC has another: Windows\' graphics settings can choose which one the game uses.')
    if 'ON BATTERY' in info.get('power', ''):
        notes.append('**This ran on battery**: the CPU and GPU do not run at full speed, so the numbers are low.')
    if 'memory_integrity=on' in info.get('vbs', ''):
        notes.append('Memory integrity (VBS) is on: it costs some CPU time; to know how much, turn it off and run again.')
    if notes:
        lines += [''] + [f'- {note}' for note in notes]
    return '\n'.join(lines)


def race_window(rows, start, end):
    """[start, end) seconds of race time that every comparable run covers.

    A fast PC plays the fixed number of race presents in a short race (4200
    presents at 120 fps are 35 seconds): then the stretch starts at a third of
    the shortest race instead, still past the countdown, rather than vanish."""
    good, _ = valid_runs(rows)
    if not good:
        return None
    shortest = min(stats['race_seconds'] for _, _, stats in good)
    end = min(end, shortest)
    if end - start < MINIMUM_WINDOW:
        start = min(start, shortest / 3)
    return (start, end) if end - start >= MINIMUM_WINDOW else None


SCENARIOS = {'race': 'a race (Free Race, with rivals and items)', 'solo': 'solo (Time Attack, no rivals)'}


def warmup_limit(info, key):
    """(presents, warm-up fps) of an 'after_say_from_warmup=N warmup_fps=F' line, which
    read_info keeps whole under its first key; None without one."""
    if not info.get(key):
        return None
    count, *rest = info[key].split()
    return count, dict(FIELD.findall(' '.join(rest))).get('warmup_fps', '?')


def method_lines(settings, info=None):
    """What was measured: the scenario, the race clock, the settings and where each run
    ended (info.txt's configs= line, and the limit the warm-up set, when it set one)."""
    info = info or {}
    scenario = settings.get('scenario')
    fixed = settings.get('fixed_step')
    configs = (info.get('configs') or '').split()
    lines = ['### How it was measured', '', '| Item | Value |', '| --- | --- |',
             f"| Scenario | {SCENARIOS.get(scenario, scenario or 'not recorded (an older benchmark.ps1: a race)')} |",
             '| Fixed step | ' + ('on: each frame advances the race 1/60 s, so every run draws the same race frames' if fixed == 'True' else
                              'off: the race follows elapsed time, so a faster setting cuts each second of race into more frames' if fixed == 'False' else
                              'not recorded (an older benchmark.ps1: off)') + ' |']
    if configs:
        repeats = settings.get('repeats', '?')
        lines.append(f"| Settings | {configs[0].replace(',', ', ')}, {repeats} round{'' if repeats == '1' else 's'} each |")
    from_warmup = warmup_limit(info, 'race_frames_from_warmup')
    longer = warmup_limit(info, 'after_say_from_warmup')
    if from_warmup:
        lines.append(f'| Each run ends | at race frame {from_warmup[0]} (race frames only; the warm-up ran {from_warmup[1]} fps,'
                     f' so this is 90 seconds of race) |')
    elif settings.get('race_frames'):
        lines.append(f"| Each run ends | at race frame {settings['race_frames']} (race frames only) |")
    elif settings.get('after_say'):
        count = longer[0] if longer else settings['after_say']
        lines.append(f"| Each run ends | {count} frames after the last menu word (the loading screen counts too) |")
    lines.append('')
    if scenario == 'solo' and fixed == 'True':
        lines.append('Use this kind of run to judge whether a change helps: no rivals or items, the same frames every run. For the frame rate a player gets, see the race (without `-FixedStep`).')
    elif scenario in (None, 'race') and fixed != 'True':
        lines.append('These numbers are the frame rate a player gets. To judge whether a change helps, use `-Scenario solo -FixedStep`: rivals and items differ every run and hide small differences.')
    else:
        lines.append('To judge whether a change helps, use `-Scenario solo -FixedStep`; for the frame rate a player gets, use a race (without `-FixedStep`).')
    return lines


def folded(title, body):
    """A block GitHub shows closed until clicked; the blank lines let markdown inside it render."""
    return f'<details>\n<summary>{title}</summary>\n\n{body}\n\n</details>'


STALL_MS = 1000  # a race frame this long is the whole game standing still, not a slow scene


def stall_note(rows):
    """The race frames of a second or more, per run: a few of them decide a run's mean
    (on an i5-3470 a Free Race once stood still for 2 to 6 seconds five times, and its
    rounds then differed by 40%), so they are named rather than averaged away."""
    found = []
    for config, repeat, stats, _ in rows:
        if not stats or config == 'warmup':
            continue
        long = [ms for _, ms in stats['timeline'] if ms >= STALL_MS]
        if long:
            found.append(f'{config}-{repeat}: {len(long)} times, {sum(long) / 1000:.1f} s in all (longest {max(long) / 1000:.1f} s)')
    if not found:
        return ''
    return '\n'.join(['**The race stopped for 1 second or more** (this, not heavier frames, is most of what lowers these runs\' mean fps and widens the spread):', '']
                     + [f'- {line}' for line in found]
                     + ['', 'While stopped the game neither draws nor advances. The cause is not in this summary (present ms and LOCK_LONG_WAIT in the log show where the time went).'
                        ' If only some runs have it, run again; if every run has it, say so in the report.'])


def summarise(directory, skip, window_start=20.0, window_end=75.0):
    """(markdown table, rows) for every <config>-<n>.log in directory."""
    runs = {}
    for log in sorted(Path(directory).glob('*.log')):
        config, _, repeat = log.stem.rpartition('-')
        if not config or not repeat.isdigit():
            continue
        runs.setdefault(config, []).append((int(repeat), log))
    # The PC first: a report pastes this file whole.
    lines = [hardware_table(directory), ''] if read_info(directory) else []
    settings = read_settings(directory)
    fixed_step = settings.get('fixed_step') == 'True'
    if settings:
        lines += method_lines(settings, read_info(directory)) + ['']
    # What decides comes first; every run's numbers and the whole-run comparison are
    # folded below them (a pasted report is long, and those are for checking).
    per_run = ['| Setting | Run | ' + ' | '.join(title for _, title, _ in COLUMNS) + ' | End |',
               '| --- | --- |' + ' ---: |' * len(COLUMNS) + ' --- |']
    rows = []
    # The warm-up first, then the settings in the order they ran.
    for config, logs in sorted(runs.items(), key=lambda item: (item[0] != 'warmup', min(p.stat().st_mtime for _, p in item[1]))):
        for repeat, log in sorted(logs):
            timeline, draw_counts = [], []
            frames, seen, ended = read_run(log, skip, timeline, fixed_step, draw_counts)
            if not frames:
                note = f'no race frames (racing frames {seen}); {ended}'
                per_run.append(f'| {config} | {repeat} | ' + ' | '.join('-' for _ in COLUMNS) + f' | {note} |')
                rows.append((config, repeat, None, ended))
                continue
            stats = describe(frames)
            stats['timeline'] = timeline
            stats['draw_counts'] = draw_counts
            stats['race_seconds'] = timeline[-1][0] if timeline else 0
            rows.append((config, repeat, stats, ended))
            per_run.append(f'| {config} | {repeat} | ' +
                           ' | '.join(fmt.format(stats[key]) for key, _, fmt in COLUMNS) + f' | {ended} |')
    whole = compare(rows)
    details = [folded(f"Every run ({len(rows)} run{'' if len(rows) == 1 else 's'})", '\n'.join(per_run))]
    window = race_window(rows, window_start, window_end)
    if window:
        start, end = window
        for _, _, stats, _ in rows:
            if stats:
                stats['window_fps'] = window_fps(stats['timeline'], start, end)
        span = (f'race seconds {start:.0f}–{end:.0f}, race frames {start * 60:.0f}–{end * 60:.0f}' if fixed_step
                else f'race seconds {start:.0f}–{end:.0f}')
        lines += ['', f'### The same stretch of race ({span})', '',
                  'Every run counts only the frames of this stretch, so a faster setting does not gain from measuring more of the lighter early race.',
                  'This table decides. Only its mean fps and round ratios are of this stretch; median, P95 and frames over 50 ms are of the whole run.'
                  ' The whole-run comparison is folded at the end.', '',
                  compare([r for r in rows if not r[2] or r[2].get('window_fps')], metric='window_fps')]
        details.append(folded('The whole run (every race frame of each run)', whole))
    else:
        lines += ['', f'(The races were too short for a common stretch of {MINIMUM_WINDOW:.0f} seconds or more, so there is no same-stretch comparison.)', '', whole]
    stalls = stall_note(rows)
    if stalls:
        lines += ['', stalls]
    lines += ['', breakdown_table(rows)]
    limit = bottleneck(rows, capped=settings.get('capped') == 'True')
    if limit:
        lines += ['', limit]
    parts = ablation_table(rows) if settings.get('capped') != 'True' else ''
    if parts:
        lines += ['', parts]
    slow = slow_stretches(rows, skip) if fixed_step else ''
    if slow:
        lines += ['', slow]
    held = held_table(directory, skip)
    if held:
        details.append(folded('What held the main thread up', held))
    lines += ['', '### Details (click to open)', ''] + [block + '\n' for block in details]
    return '\n'.join(lines).rstrip() + '\n', rows


MINIMUM_ROUNDS = 4  # fewer pairs than this prove nothing, whatever the ratios
EXPECTED_FRAMES_TOLERANCE = 0.02  # a run with fewer race frames than the others did not play the same race


def valid_runs(rows):
    """(config, repeat, stats) of the runs that can be compared, and why the others cannot."""
    counts = [stats['frames'] for config, _, stats, ended in rows
              if config != 'warmup' and stats and 'present-limit' in ended]
    expected = statistics.median(counts) if counts else 0
    good, problems = [], []
    for config, repeat, stats, ended in rows:
        if config == 'warmup':
            continue
        if not stats:
            problems.append(f'{config}-{repeat}: never reached the race, no race frames to measure ({ended})')
        elif 'present-limit' not in ended:
            problems.append(f'{config}-{repeat}: did not reach present-limit ({ended})')
        elif abs(stats['frames'] - expected) > EXPECTED_FRAMES_TOLERANCE * expected:
            problems.append(f"{config}-{repeat}: {stats['frames']} race frames, the other runs about {expected:.0f}")
        else:
            good.append((config, repeat, stats))
    return good, problems


def compare(rows, reference_name=None, metric='fps'):
    """Each setting against baseline (or --reference, or exe-a when there is no baseline): the median run, then round by round.

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
        return 'No run can be compared.\n' + '\n'.join(f'- {p}' for p in problems)
    lines = []
    if problems:
        lines += ['**Left out of the comparison:**'] + [f'- {p}' for p in problems] + ['']
    median = {config: {key: statistics.median(s[key] for s in runs.values()) for key in (metric, 'median_ms', 'p95_ms', 'slow50')}
              for config, runs in by_config.items()}
    # The reference is baseline; an exe-a / exe-b comparison has none, so exe-a stands in.
    ref = reference_name or ('baseline' if 'baseline' in by_config else 'exe-a' if 'exe-a' in by_config else sorted(by_config)[0])
    if ref not in by_config:
        return '\n'.join(lines + [f'There is no {ref} to compare against.'])
    reference = median.get(ref)
    lines += [f'| Setting | Runs | Mean fps | Median ms | P95 ms | Frames >50 ms | vs {ref} |', '| --- | ---: | ---: | ---: | ---: | ---: | ---: |']
    for config, value in median.items():
        change = f"{(value[metric] / reference[metric] - 1) * 100:+.0f}%" if reference else '-'
        lines.append(f"| {config} | {len(by_config[config])} | {value[metric]:.1f} | {value['median_ms']:.1f} | "
                     f"{value['p95_ms']:.1f} | {value['slow50']:.0f} | {change} |")
    baseline = by_config.get(ref, {})
    if len(baseline) >= 2:
        fps = [s[metric] for s in baseline.values()]
        spread = (max(fps) - min(fps)) / statistics.median(fps)
        lines += ['', f'Spread of {ref} itself between runs (largest minus smallest, over the median): **{spread * 100:.0f}%**'
                      f' ({min(fps):.1f}–{max(fps):.1f} fps). Another setting\'s difference counts only when clearly larger than this.']
        paired = []
        for config, runs in by_config.items():
            if config == ref:
                continue
            rounds = sorted(set(runs) & set(baseline))
            if len(rounds) < 2:
                continue
            ratios = [runs[r][metric] / baseline[r][metric] for r in rounds]
            middle = statistics.median(ratios)
            faster = sum(1 for r in ratios if r > 1)
            consistent = max(faster, len(ratios) - faster) / len(ratios) >= 0.8
            threshold = max(0.05, spread / 2)
            if len(ratios) < MINIMUM_ROUNDS:
                verdict = f'too few rounds (a verdict needs {MINIMUM_ROUNDS})'
            elif abs(middle - 1) > threshold and consistent:
                verdict = 'faster' if middle > 1 else 'slower'
            else:
                verdict = 'no verdict (inside the noise)'
            paired.append(f"| {config} | {' '.join(f'{r:.2f}' for r in ratios)} | {middle:.2f} | {faster}/{len(ratios)} | {verdict} |")
        if paired:
            lines += ['', f'Paired by round (the setting\'s fps over {ref}\'s fps in the same round):', '',
                      '| Setting | Ratio each round | Median | Rounds faster | Verdict |', '| --- | --- | ---: | ---: | --- |'] + paired
    else:
        lines += ['', f'{ref} has fewer than two runs: no noise estimate and no paired rounds.']
    return '\n'.join(lines)


# Where a frame's time goes on the main thread, from the frame metrics. The
# fields overlap, so each part is taken once: the GPU wait happens inside the
# present and inside the main thread's waits, and the frame cap's wait is one of
# those waits too. What is left is the main thread running: the game's own code,
# the HLE imports and hooks, and whatever else is not measured apart.
PARTS = (('running', 'Main thread running (game code, HLE)'), ('draw_ms', 'Drawing (CPU side)'), ('gpu_wait_ms', 'Waiting for the GPU'),
         ('present', 'Present (less the GPU wait)'), ('wait', 'Other waits'), ('main_queued_ms', 'Queued for a lock'),
         ('pacing_ms', 'Frame-limit wait'))
TARGETS = (60, 120)  # the console's rate, and the cap fast PCs are compared at


def frame_parts(stats):
    """{part: ms} of one run's mean frame, the parts adding up to it.

    The GPU wait is mostly inside the present, but a draw waits for the GPU
    too when the command ring fills mid-frame (native_renderer.cpp's ring
    flushes), and that wait is inside draw_ms: what exceeds the present comes
    out of the draw."""
    frame = 1000 / stats['fps']
    gpu, pacing = stats['gpu_wait_ms'], stats['pacing_ms']
    gpu_in_present = min(gpu, stats['present_ms'])
    parts = {'draw_ms': max(0.0, stats['draw_ms'] - (gpu - gpu_in_present)), 'gpu_wait_ms': gpu,
             'present': stats['present_ms'] - gpu_in_present,
             'wait': max(0.0, stats['main_blocked_ms'] - gpu - pacing), 'main_queued_ms': stats['main_queued_ms'],
             'pacing_ms': pacing}
    parts['running'] = max(0.0, frame - sum(parts.values()))
    return frame, parts


def breakdown_table(rows):
    """Each setting's median frame split into PARTS, in ms and as a share of the frame."""
    good, _ = valid_runs(rows)
    by_config = {}
    for config, _, stats in good:
        by_config.setdefault(config, []).append(frame_parts(stats))
    if not by_config:
        return ''
    lines = ['### Where the main thread\'s frame goes', '',
             'Judge improvements in ms: a saving is a fixed number of milliseconds, and the same 2 ms is a small share on a slow PC and a large one on a fast PC. The percentages are for comparing the kind of limit with other PCs.', '',
             '| Setting | ms a frame | ' + ' | '.join(title for _, title in PARTS) + ' |',
             '| --- | ---: |' + ' ---: |' * len(PARTS)]
    for config, runs in by_config.items():
        frame = statistics.median(f for f, _ in runs)
        cells = []
        for key, _ in PARTS:
            ms = statistics.median(p[key] for _, p in runs)
            cells.append(f'{ms:.1f} ({ms / frame * 100:.0f}%)')
        lines.append(f'| {config} | {frame:.1f} | ' + ' | '.join(cells) + ' |')
    lines += ['', 'From every frame measured in the run (less the first --skip). The parts do not overlap and add up to the frame: the GPU wait, part of present and of the main thread\'s waits,'
                  ' and the frame-limit wait, part of the waits, are counted once. "Main thread running" is what is left after the others.']
    if all(s['main_blocked_ms'] == 0 for _, _, s in good):
        lines += ['', '**This executable did not record the main thread\'s waits** (main_blocked_ms is all 0: an older build did not count them when the main thread runs without the permit),'
                      ' so the waits are in "Main thread running".']
    return '\n'.join(lines)


def bottleneck(rows, capped=False):
    """What limits this PC, from baseline against skip-draws and render-50 (run_benchmark.bat report)."""
    good, _ = valid_runs(rows)
    by_config = {}
    for config, _, stats in good:
        by_config.setdefault(config, []).append(stats)
    if not {'baseline', 'skip-draws', 'render-50'} <= set(by_config):
        return ''
    if capped:
        return ('### What limits this PC\n\nThis ran with a frame limit (-Capped): all three may sit at the limit, so the ratios say nothing about it.'
                ' Run run_benchmark.bat report again without -Capped.')
    metric = 'window_fps' if all(s.get('window_fps') for runs in by_config.values() for s in runs) else 'fps'
    fps = {config: statistics.median(s[metric] for s in runs) for config, runs in by_config.items()}
    base, ceiling, half = fps['baseline'], fps['skip-draws'], fps['render-50']
    # Each setting against the baseline of its own round, as compare() does: a
    # laptop that warms up over the rounds slows every setting alike, and the
    # paired ratio does not carry that. Fewer than two pairs: the medians.
    by_round = {}
    for config, repeat, stats in good:
        by_round.setdefault(config, {})[repeat] = stats[metric]
    def ratio(config):
        rounds = sorted(set(by_round[config]) & set(by_round['baseline']))
        if len(rounds) < 2:
            return fps[config] / base
        return statistics.median(by_round[config][r] / by_round['baseline'][r] for r in rounds)
    ceiling_ratio, half_ratio = ratio('skip-draws'), ratio('render-50')
    if half_ratio >= 1.10:
        verdict = f'**GPU**: half resolution is {(half_ratio - 1) * 100:.0f}% faster; lowering the rendering resolution helps most.'
        decisive = half_ratio
    elif ceiling_ratio >= 1.15:
        verdict = (f'**drawing path**: no drawing is {(ceiling_ratio - 1) * 100:.0f}% faster but half resolution makes almost no difference,'
                   ' so the cost is drawing and submitting on the CPU, not the GPU\'s pixels.')
        decisive = ceiling_ratio
    else:
        verdict = ('**the game\'s own CPU time**: neither no drawing nor half resolution is much faster,'
                   ' so the main thread\'s game code and HLE are the limit, and only a faster core helps.')
        decisive = None
    # The gap that decided it has to clear the baseline's own run-to-run spread
    # (half of it, as compare() asks), or the verdict is a guess.
    baseline_runs = [by_round['baseline'][r] for r in sorted(by_round['baseline'])]
    spread = (max(baseline_runs) - min(baseline_runs)) / statistics.median(baseline_runs) if len(baseline_runs) >= 2 else 0.0
    cautions = []
    if decisive and decisive - 1 < spread / 2:
        cautions.append(f'**The verdict is unreliable**: the difference that decides it ({(decisive - 1) * 100:.0f}%) is not clearly larger than the baseline\'s own spread'
                        f' ({spread * 100:.0f}%). Run more rounds, or quiet the PC first.')
    # Heat shows as the later rounds slower than the earlier ones (halves, so a
    # PC that is merely noisy from round to round does not look like one warming up).
    if len(baseline_runs) >= 3:
        rounds = len(baseline_runs) // 2
        earlier, later = statistics.fmean(baseline_runs[:rounds]), statistics.fmean(baseline_runs[-rounds:])
        if later < 0.95 * earlier:
            cautions.append(f'The baseline\'s later rounds are slower than its earlier ones (mean {earlier:.1f} → {later:.1f} fps): it looks like thermal throttling.'
                            ' Plug a laptop in, use its highest performance mode and let it cool before running; the round ratios already allow for this, so the verdict stands.')
    frame = 1000 / base
    target = next((t for t in TARGETS if base < t), None)
    if target:
        reached = [t for t in TARGETS if t < target]
        gap = (f'A frame takes {frame:.1f} ms now; {target} fps needs {1000 / target:.1f} ms: {frame - 1000 / target:.1f} ms a frame still to save'
               f' ({(frame * target / 1000 - 1) * 100:.0f}% faster). The breakdown above shows where it can come from.'
               + (f' (Already past {reached[-1]} fps.)' if reached else ''))
    else:
        gap = f'A frame takes {frame:.1f} ms now, past {TARGETS[-1]} fps.'
    which = 'same stretch of race' if metric == 'window_fps' else 'whole run'
    return '\n'.join(['### What limits this PC', '',
                      f'| | baseline | skip-draws (no drawing: the CPU\'s ceiling) | render-50 (half resolution) |',
                      '| --- | ---: | ---: | ---: |',
                      f'| fps, {which} (median; in brackets the median round ratio) | {base:.1f} | {ceiling:.1f} ({(ceiling_ratio - 1) * 100:+.0f}%) | '
                      f'{half:.1f} ({(half_ratio - 1) * 100:+.0f}%) |', '',
                      f'Verdict: {verdict}', ''] + [f'{c}\n' for c in cautions] + [gap, '',
                      'Thresholds: half resolution 10% faster or more is the GPU; otherwise no drawing 15% faster or more is the drawing path; neither is the game\'s own CPU time.'
                      ' Frame rates of different PCs cannot be compared directly, but these ratios can.'])


# Settings that take one part away (or put back what an optimization saves), in
# the order the table lists them: (config, what changes, what the difference says).
ABLATIONS = (
    ('skip-draws', 'nothing drawn', 'drawing (CPU and GPU) made free would save at most'),
    ('render-every-2', 'every other frame drawn', 'about half the cost of drawing'),
    ('render-50', 'three quarters fewer pixels', 'the GPU\'s cost of pixels'),
    ('no-audio', 'no sound output (the game still mixes)', 'the cost of sound output on the PC'),
    ('serial', 'guest threads one at a time', 'read the other way: what running in parallel saves now'),
    ('no-vertex-cache', 'vertex cache off', 'read the other way: what this optimization saves on this PC'),
    ('no-gpu-pipeline', 'GPU pipelining off', 'read the other way: what this optimization saves on this PC'),
    ('main-unpinned', 'main thread not pinned to the first core', 'read the other way: what pinning the main thread saves on this PC'),
    ('main-pinned', 'main thread pinned to the first core', 'read the other way: what leaving the main thread unpinned saves on this PC'),
)
# Those that turn an optimization off: slower is what the optimization saves.
REVERSED = {'serial', 'no-vertex-cache', 'no-gpu-pipeline', 'main-unpinned', 'main-pinned'}


def ablation_table(rows):
    """What each part costs a frame here: every ablation setting against the baseline of its own round, in ms."""
    good, _ = valid_runs(rows)
    present = {config for config, _, _ in good}
    listed = [a for a in ABLATIONS if a[0] in present]
    if 'baseline' not in present or not listed:
        return ''
    metric = 'window_fps' if all(stats.get('window_fps') for _, _, stats in good) else 'fps'
    by_round = {}
    for config, repeat, stats in good:
        by_round.setdefault(config, {})[repeat] = stats[metric]
    base_runs = list(by_round['baseline'].values())
    base_ms = 1000 / statistics.median(base_runs)
    spread = (max(base_runs) - min(base_runs)) / statistics.median(base_runs) if len(base_runs) >= 2 else 0.0
    noise = max(0.02, spread / 2)
    lines = ['### What each part costs', '',
             f'Each setting changes one thing and is compared with the same round\'s baseline (the median round ratio), in ms a frame; the baseline takes {base_ms:.1f} ms a frame.'
             ' This measures cause: what is saved is that part\'s cost on this PC, so the parts can be ranked by it.', '',
             '| Setting | What changes | ms a frame | vs baseline | Ratio each round | Reading |', '| --- | --- | ---: | ---: | --- | --- |']
    for config, change, meaning in listed:
        rounds = sorted(set(by_round[config]) & set(by_round['baseline']))
        if rounds:
            ratios = [by_round[config][r] / by_round['baseline'][r] for r in rounds]
            ratio = statistics.median(ratios)
        else:
            ratios, ratio = [], statistics.median(by_round[config].values()) / statistics.median(base_runs)
        ms = base_ms / ratio
        saved = base_ms - ms
        if abs(ratio - 1) < noise:
            reading = f'inside the noise (the baseline\'s runs differ by {spread * 100:.0f}%): no visible cost'
        elif config in REVERSED:
            reading = f'{meaning}: {-saved:.1f} ms' if saved < 0 else f'turned off it is {saved:.1f} ms faster: this optimization costs on this PC'
        else:
            reading = f'{meaning}: {saved:.1f} ms' if saved > 0 else f'{-saved:.1f} ms slower instead'
        lines.append(f'| {config} | {change} | {ms:.1f} | {-saved:+.1f} ms | {" ".join(f"{r:.2f}" for r in ratios) or "-"} | {reading} |')
    lines += ['', f'Noise threshold: a difference under {noise * 100:.0f}% (half the baseline\'s spread, at least 2%) does not count.'
              + (' From the fps of the same stretch of race.' if metric == 'window_fps' else ' From whole-run fps (no common stretch of race).')]
    return '\n'.join(lines)


SLOW_BLOCK = 30  # race frames a stretch: half a second of race with a fixed step


def slow_stretches(rows, skip, top=8):
    """The heaviest stretches of the race in the baseline runs, by race frame (a fixed
    step only: then frame N is the same moment of the race in every run and on every PC,
    so the author can go to it). A stretch counts where most runs agree it is slow."""
    good, _ = valid_runs(rows)
    runs = [stats for config, _, stats in good if config == 'baseline']
    if len(runs) < 2:
        return ''
    per_run = []
    for stats in runs:
        blocks, mean = {}, 1000 / stats['fps']
        for (t, ms), drawn in zip(stats['timeline'], stats['draw_counts']):
            frame = round(t * 60)
            if frame >= skip:
                blocks.setdefault(frame // SLOW_BLOCK, []).append((ms, drawn))
        per_run.append(({b: (statistics.fmean(m for m, _ in v), statistics.median(d for _, d in v))
                         for b, v in blocks.items() if len(v) == SLOW_BLOCK}, mean))
    common = set.intersection(*(set(blocks) for blocks, _ in per_run))
    found = []
    for block in common:
        ms = statistics.median(blocks[block][0] for blocks, _ in per_run)
        heavy = sum(1 for blocks, mean in per_run if blocks[block][0] > 1.25 * mean)
        if heavy * 2 > len(per_run):
            found.append((ms, block, heavy, statistics.median(blocks[block][1] for blocks, _ in per_run)))
    whole = statistics.median(mean for _, mean in per_run)
    lines = ['### The slowest stretches of the race', '',
             f'With a fixed step, race frame N is the same moment in every run and on every PC: the same save reaches the same scene at that frame.'
             f' In blocks of {SLOW_BLOCK} frames (half a second), these are the blocks at least 25% heavier than the race average ({whole:.1f} ms) in most baseline runs; adjacent blocks are merged.', '']
    if not found:
        return '\n'.join(lines + ['No stretch is heavy in most runs: the slow frames are scattered, not one scene.'])
    # Neighbouring stretches are one scene: one row each, ranked by its heaviest stretch.
    scenes = []
    for ms, block, heavy, drawn in sorted(found, key=lambda f: f[1]):
        if scenes and scenes[-1]['last'] == block - 1:
            scene = scenes[-1]
            scene['last'] = block
            scene['ms'].append(ms)
            scene['heavy'] = min(scene['heavy'], heavy)
            scene['drawn'].append(drawn)
        else:
            scenes.append({'first': block, 'last': block, 'ms': [ms], 'heavy': heavy, 'drawn': [drawn]})
    lines += ['| Race frames | Race time | ms a frame (mean / heaviest half second) | vs race average | Heavy runs | Draws a frame |', '| --- | --- | ---: | ---: | ---: | ---: |']
    for scene in sorted(scenes, key=lambda s: -max(s['ms']))[:top]:
        first, end = scene['first'] * SLOW_BLOCK, (scene['last'] + 1) * SLOW_BLOCK
        mean, peak = statistics.fmean(scene['ms']), max(scene['ms'])
        lines.append(f'| {first}–{end - 1} | {first / 60:.1f}–{end / 60:.1f} s | {mean:.1f} / {peak:.1f} | '
                     f'{(mean / whole - 1) * 100:+.0f}% | {scene["heavy"]}/{len(per_run)} | {statistics.median(scene["drawn"]):.0f} |')
    return '\n'.join(lines)


# The parts of run_benchmark.bat full (scripts/benchmark_all.ps1), in the order report.md gives them.
REPORT_PARTS = (('race', 'The frame rate a player gets (Free Race with rivals, elapsed time)'),
                ('parts', 'What each part costs and what limits this PC (Time Attack, fixed step)'))


def combine_report(directory, minutes=None):
    """report.md of a full run: the PC once (from the first part's summary that has it), then
    each part's summary from its method on. Kept here, not in the PowerShell script, because
    Windows PowerShell reads a script without a byte order mark in the system's code page."""
    texts = {name: (Path(directory) / name / 'summary.md').read_text(encoding='utf-8')
             for name, _ in REPORT_PARTS if (Path(directory) / name / 'summary.md').is_file()}
    lines = ['## Performance report (run_benchmark.bat full' + (f', {minutes} minutes' if minutes is not None else '') + ')', '']
    hardware = next((text[:text.index('### How it was measured')].strip() for text in texts.values()
                     if '### Hardware and drivers' in text and '### How it was measured' in text), '')
    if hardware:
        lines += [hardware, '']
    for name, title in REPORT_PARTS:
        if name not in texts:
            lines += [f'## {title}', '', '(This part did not finish.)', '']
            continue
        text = texts[name]
        at = text.find('### How it was measured')
        lines += [f'## {title}', '', (text[at:] if at >= 0 else text).strip(), '']
    report = '\n'.join(lines).rstrip() + '\n'
    (Path(directory) / 'report.md').write_text(report, encoding='utf-8')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('directory')
    parser.add_argument('--report', action='store_true', help='directory is a full run (benchmark_all.ps1): write its report.md')
    parser.add_argument('--minutes', type=int, help='how long the full run took, for report.md')
    parser.add_argument('--skip', type=int, default=600, help='race frames left out at the start (default 600)')
    parser.add_argument('--window-start', type=float, default=20.0, help='race seconds where the common stretch begins')
    parser.add_argument('--window-end', type=float, default=75.0, help='race seconds where it ends at the latest')
    args = parser.parse_args()
    if args.report:
        print(combine_report(args.directory, args.minutes))
        return
    table, _ = summarise(args.directory, args.skip, args.window_start, args.window_end)
    print(table)
    (Path(args.directory) / 'summary.md').write_text(table + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
