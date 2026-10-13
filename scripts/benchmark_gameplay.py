"""Validate all countdown + racing ticks, reporting each stage and combined cost."""
import argparse
import json
import math
from pathlib import Path
import re
import statistics
from benchmark_summary import present_record


def metrics(values):
    values = sorted(values)
    if not values:
        raise ValueError('empty stage/window')
    mean = statistics.mean(values)
    return dict(frames=len(values), fps=1000/mean, mean_ms=mean,
                p95_ms=values[math.ceil(.95*len(values))-1],
                p99_ms=values[math.ceil(.99*len(values))-1], max_ms=values[-1],
                over_33_33=sum(v >= 1000/30 for v in values))


def inspect_gameplay(path, start=1200, end=4500, limit=5400):
    with Path(path).open(encoding='utf-8-sig', errors='replace') as stream:
        return _inspect_lines(stream, start, end, limit)


def _inspect_lines(lines, start, end, limit):
    if not 0 <= start < end <= limit-300:
        raise ValueError('require 0 <= race-window start < end <= limit - 300 countdown ticks')
    run = config = previous_frame = None
    previous_ms, remaining, tick = 0.0, 5000.0, 0
    stopped = False
    countdown, racing, window = [], [], []
    first_present = last_present = None
    for raw in lines:
        if raw.startswith('STOP '):
            if stopped or not raw.startswith('STOP gameplay-limit ') or tick != limit:
                raise ValueError('unexpected or incomplete stop')
            stopped = True
            continue
        if raw.startswith('HANG_REPORT') or 'NATIVE_GRAPHICS_DRED page_fault=' in raw or re.search(r'managed-[a-z0-9-]+-mismatch', raw):
            raise ValueError('correctness failure in log')
        line = present_record(raw)
        if not line:
            continue
        if stopped:
            raise ValueError('present after stop')
        row = dict(token.split('=', 1) for token in line.split()[1:] if '=' in token)
        if row.get('gameplay_schema') != '1' or row.get('game_contract') != 'solo_fixed_v1':
            raise ValueError('missing supported progress; legacy racing=1 is insufficient')
        if row.get('race_probe', '0') != '0' or row.get('draw_timers') != '0' or row.get('presented') != '1':
            raise ValueError('raw diagnostic, draw timers or omitted present')
        frame = int(row['frame'])
        if (previous_frame is None and frame != 0) or (previous_frame is not None and frame != previous_frame+1):
            raise ValueError('missing or repeated present')
        previous_frame = frame
        if run is None and row['game_state'] in ('unseen', 'prestart'):
            continue
        if row['game_state'] != 'advancing' or row.get('game_start_observed') != '1':
            raise ValueError('pause, phase exit or unobserved/invalid progress')
        current_tick = int(row['game_tick'])
        if current_tick != tick+1 or current_tick > limit:
            raise ValueError('missing, duplicate or excessive simulation tick')
        current_config = tuple(int(v) for v in row['game_config'].split(':'))
        if run is None:
            run, config, first_present = int(row['game_run']), current_config, frame
            if run <= 0 or len(config) != 7 or config[6] != 0:
                raise ValueError('invalid run epoch/configuration')
        if int(row['game_run']) != run or current_config != config:
            raise ValueError('race epoch or configuration changed')
        duration = float(row['frame_ms'])
        elapsed, current_remaining = float(row['game_elapsed_ms']), float(row['game_countdown_ms'])
        if not all(math.isfinite(v) and v >= 0 for v in (duration, elapsed, current_remaining)) or duration == 0:
            raise ValueError('invalid timer/present interval')
        cd_tick, race_tick, phase = int(row['game_countdown_tick']), int(row['game_race_tick']), int(row['game_phase'])
        if current_tick <= 300:
            # Final countdown decrement publishes phase11/elapsed0; retain it.
            if (row['game_stage'] != 'countdown' or cd_tick != current_tick or race_tick != 0 or
                phase != (11 if cd_tick == 300 else 10) or abs(elapsed) > .001 or
                (cd_tick == 300 and current_remaining > .001) or
                abs((remaining-current_remaining)-1000/60) > .002):
                raise ValueError('invalid or incomplete countdown interval')
            countdown.append(duration)
        else:
            if (row['game_stage'] != 'racing' or cd_tick != 300 or race_tick != current_tick-300 or
                phase != 11 or current_remaining > .001 or abs((elapsed-previous_ms)-1000/60) > .002):
                raise ValueError('invalid racing interval')
            racing.append(duration)
            if start < race_tick <= end:
                window.append(duration)
        tick, previous_ms, remaining, last_present = current_tick, elapsed, current_remaining, frame
    if not stopped or tick != limit or len(countdown) != 300 or len(window) != end-start:
        raise ValueError('incomplete countdown + racing segment/window')
    return dict(progress_valid=True, contract='solo_fixed_v1', run=run, config=config,
                first_present=first_present, last_present=last_present, simulation_ticks=tick,
                countdown=metrics(countdown), racing=metrics(racing), combined=metrics(countdown+racing),
                race_window=dict(start_tick=start, end_tick=end, **metrics(window)))


def inspect_runs(paths, start=1200, end=4500, limit=5400):
    results = {str(path): inspect_gameplay(path, start, end, limit) for path in paths}
    if len({tuple(row['config']) for row in results.values()}) != 1:
        raise ValueError('different race configurations; do not compare')
    return dict(purpose='Progress integrity; hashes/settings/route, input response and display acceptance remain separate', runs=results)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', nargs='+', type=Path)
    parser.add_argument('--start', type=int, default=1200)
    parser.add_argument('--end', type=int, default=4500)
    parser.add_argument('--limit', type=int, default=5400, help='total ticks INCLUDING300 countdown ticks')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    text = json.dumps(inspect_runs(args.logs, args.start, args.end, args.limit), indent=2)
    if args.output:
        args.output.write_text(text+'\n', encoding='utf-8')
    print(text)


if __name__ == '__main__':
    main()
