"""Summarize a fixed present-index window; never change game timing or settings."""
import argparse
import json
import math
import statistics
from pathlib import Path


def distribution(values):
    values = sorted(values)
    if not values:
        return None
    return dict(mean=statistics.mean(values), p50=statistics.median(values),
                p95=values[max(0, math.ceil(len(values) * .95) - 1)],
                p99=values[max(0, math.ceil(len(values) * .99) - 1)])


def scene_anchor(lines, draws):
    """Course-specific heuristic; screenshots must confirm the same scene."""
    for line in lines:
        if line.startswith('NATIVE_PRESENT '):
            row = dict(token.split('=', 1) for token in line.split()[1:] if '=' in token)
            if int(row.get('draws', 0)) > draws:
                return int(row['frame'])
    raise ValueError('no frame exceeds the scene draw threshold')


def summarize(lines, first, last):
    rows = []
    for line in lines:
        if not line.startswith('NATIVE_PRESENT '):
            continue
        row = dict(token.split('=', 1) for token in line.split()[1:] if '=' in token)
        frame = int(row['frame'])
        if first <= frame <= last:
            row['frame'] = frame
            rows.append(row)
    intervals, work, gaps = [], [], 0
    measured = []
    for a, b in zip(rows, rows[1:]):
        if b['frame'] != a['frame'] + 1:
            gaps += 1
            continue
        duration = float(b['frame_ms']) if 'frame_ms' in b else (float(b['seconds']) - float(a['seconds'])) * 1000
        if not math.isfinite(duration) or duration <= 0:
            raise ValueError('frame intervals must be finite and positive')
        intervals.append(duration)
        measured.append(b)
        if 'pacing_ms' in b:
            pacing = float(b['pacing_ms'])
            if not math.isfinite(pacing) or pacing < 0 or pacing > duration + .001:
                raise ValueError('pacing duration must be within its frame interval')
            work.append(max(0, duration - pacing))
    if not intervals:
        raise ValueError('no consecutive frame intervals in the selected window')
    fields = ('draws', 'draw_ms', 'pipeline_ms', 'texture_ms', 'present_ms', 'gpu_wait_ms',
              'main_queued_ms', 'main_ready_unowned_ms', 'pacing_ms')
    return dict(first_frame=rows[0]['frame'], last_frame=rows[-1]['frame'],
                intervals=len(intervals), missing_intervals=gaps,
                fps=1000 / statistics.mean(intervals), frame_ms=distribution(intervals),
                non_pacing_ms=distribution(work) if len(work) == len(intervals) else None,
                over_16_667_ms=sum(v > 1000 / 60 for v in intervals),
                over_33_333_ms=sum(v > 1000 / 30 for v in intervals),
                pipeline_compile_frames=sum(float(r.get('pipeline_ms', 0)) > 0 for r in measured),
                metrics={k: distribution([float(r[k]) for r in measured if k in r]) for k in fields})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', nargs='+', type=Path)
    parser.add_argument('--first', type=int, required=True)
    parser.add_argument('--last', type=int, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--scene-draws', type=int,
                        help='treat first/last as offsets from the first frame with more than this many draws; verify scene manually')
    args = parser.parse_args()
    if args.last <= args.first:
        parser.error('--last must be greater than --first')
    report = {}
    for log in args.logs:
        lines = log.read_text(errors='replace').splitlines()
        anchor = scene_anchor(lines, args.scene_draws) if args.scene_draws is not None else 0
        first, last = anchor + args.first, anchor + args.last
        result = summarize(lines, first, last)
        if args.scene_draws is not None:
            result['scene_anchor'] = anchor
        result['stops'] = [s for s in lines if s.startswith(('STOP ', 'HANG_REPORT', 'RUNTIME_STOP'))]
        result['complete_window'] = result['first_frame'] == first and result['last_frame'] == last and not result['missing_intervals']
        report[str(log)] = result
    text = json.dumps(report, indent=2)
    if args.output:
        args.output.write_text(text + '\n', encoding='utf-8')
    print(text)


if __name__ == '__main__':
    main()
