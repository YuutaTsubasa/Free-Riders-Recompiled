"""Validate i5 data integrity and summarize its legacy race-object window."""
import argparse
import json
import math
import re
import statistics
from pathlib import Path

from benchmark_summary import read_run


def inspect_run(directory, config, repeat, suite):
    label = f'{config}-{repeat}'
    result = dict(stage=directory.name, config=config, repeat=repeat, errors=[],
                  window_basis='legacy_race_object_present', gameplay_alignment_verified=False)
    errors = result['errors']
    log, metadata = directory / f'{label}.log', directory / f'{label}.json'
    if not log.is_file() or not metadata.is_file():
        errors.append('Missing log or executable/environment metadata')
        return result
    meta = json.loads(metadata.read_text(encoding='utf-8-sig'))
    old = config in ('codex-before', 'codex-control')
    expected_hash = suite['baseline_sha256' if old else 'candidate_sha256']
    result['sha256'] = meta.get('sha256', '')
    if result['sha256'].lower() != expected_hash.lower():
        errors.append('Executable hash differs from the kit manifest')
    settings = meta.get('settings', {})
    expected = dict(SFR_DRAW_TIMERS='0', SFR_FRAME_METRICS='1', SFR_REALTIME_RACE='0',
                    SFR_RENDER_EVERY='1', SFR_AUDIO='1', SFR_FRAME_LIMIT='0',
                    SFR_GRAPHICS='d3d12', SFR_WINDOW_WIDTH='1280', SFR_WINDOW_HEIGHT='720',
                    SFR_PRESENT_LIMIT_RACING=str(suite['race_frames']))
    for key, value in expected.items():
        if settings.get(key) != value:
            errors.append(f'Unexpected {key}: {settings.get(key)!r}')
    # Old suites predate the independent texture-state flag. Their candidate
    # settings remain valid only with that feature disabled, not silently mixed
    # with the new checkpoint's settings under the same configuration names.
    texture_state = suite.get('managed_texture_state', False)
    stencil_state = suite.get('managed_stencil_state', False)
    flags = dict(SFR_MANAGED_CONSTANTS=not old,
                 SFR_MANAGED_BUFFERS=config in ('managed-buffers', 'verify-buffers'),
                 SFR_MANAGED_TEXTURE_STATE=texture_state and not old,
                 SFR_VERIFY_MANAGED_TEXTURE_STATE=texture_state and config == 'verify-buffers',
                 SFR_MANAGED_STENCIL_STATE=stencil_state and not old,
                 SFR_VERIFY_MANAGED_STENCIL_STATE=stencil_state and config == 'verify-buffers',
                 SFR_VERIFY_MANAGED_BUFFERS=config == 'verify-buffers',
                 SFR_D3D12_DRED=config == 'verify-buffers')
    if texture_state and config == 'verify-buffers':
        flags['SFR_VERIFY_MANAGED_CONSTANTS'] = True
    for key, enabled in flags.items():
        if settings.get(key, '0') != ('1' if enabled else '0'):
            errors.append(f'Unexpected {key}')
    for key in ('SFR_SKIP_DRAWS', 'SFR_VERIFY_MANAGED_CONSTANTS', 'SFR_VERIFY_MANAGED_TEXTURE_STATE',
                'SFR_VERIFY_MANAGED_STENCIL_STATE',
                'SFR_BUFFER_AUDIT', 'SFR_SCREENSHOT_EVERY', 'SFR_UW_DEBUG', 'SFR_UW_ISOLATE', 'SFR_RACE_PROGRESS',
                'SFR_GAMEPLAY_METRICS', 'SFR_GAMEPLAY_TICK_LIMIT', 'SFR_DRAW_SIGNATURE_TRACE',
                'SFR_TEXTURE_TIMERS', 'SFR_TEXTURE_TRACE'):
        if config != 'verify-buffers' and settings.get(key, '0') != '0':
            errors.append(f'Diagnostic setting present in measured run: {key}')
    if settings.get('SFR_RENDER_SCALE', '100') != '100':
        errors.append('Rendering scale differs from 100%')
    timeline = []
    _, raced, ended = read_run(log, suite['skip'], timeline=timeline, fixed_step=True)
    result.update(racing_frames=raced, ended=ended)
    if raced != suite['race_frames'] or ended != 'present-limit':
        errors.append(f'Incomplete run: {raced} racing frames, {ended}')
    with log.open(encoding='utf-8-sig', errors='replace') as stream:
        for line in stream:
            if re.search(r'managed-[a-z0-9-]+-mismatch|^HANG_REPORT|NATIVE_GRAPHICS_DRED page_fault=', line):
                errors.append('Correctness/GPU failure: ' + line.strip()[:200])
                break
    # Timeline positions are zero-based racing-frame indices, exactly 1200..4499.
    values = [ms for seconds, ms in timeline if suite['skip'] <= round(seconds * 60) < suite['window_end']]
    if len(values) != suite['window_end'] - suite['skip'] or any(not math.isfinite(v) or v <= 0 for v in values):
        errors.append('Missing or invalid frame intervals in the common window')
    if not errors and config != 'verify-buffers':
        values.sort()
        result.update(frames=len(values), mean_ms=statistics.mean(values), fps=1000 / statistics.mean(values),
                      p95_ms=values[math.ceil(.95 * len(values)) - 1], p99_ms=values[math.ceil(.99 * len(values)) - 1],
                      over_16_67=sum(v > 1000 / 60 for v in values),
                      over_33_33=sum(v >= 1000 / 30 for v in values), over_50=sum(v > 50 for v in values))
    return result


def build_report(directory):
    directory = Path(directory)
    suite = json.loads((directory / 'suite.json').read_text(encoding='utf-8-sig'))
    stages = [('verify', ['verify-buffers'], 1),
              ('control', ['codex-before', 'codex-control'], suite['control_repeats']),
              ('compare', ['codex-before', 'codex-after', 'managed-buffers'], suite['repeats'])]
    runs = []
    for stage, configs, repeats in stages:
        for repeat in range(1, repeats + 1):
            for config in configs:
                try:
                    runs.append(inspect_run(directory / stage, config, repeat, suite))
                except (ValueError, OSError, KeyError, OverflowError) as error:
                    runs.append(dict(stage=stage, config=config, repeat=repeat, errors=[str(error)]))
    valid = suite['status'] == 'completed' and not any(run['errors'] for run in runs)
    verdict = 'COMPLETE - measurements collected; NOT a stable-60-Hz acceptance' if valid else 'INCOMPLETE / FAILED - do not treat as a performance verdict'
    lines = ['# Codex i5 measurements', '', verdict, '',
             f"Candidate commit: `{suite['candidate_commit']}`.", '',
             f"Candidate SHA-256: `{suite['candidate_sha256']}`.", '',
             f"Baseline SHA-256: `{suite['baseline_sha256']}`.", '',
             'Candidate texture-state cache: ' + ('enabled.' if suite.get('managed_texture_state', False)
                                                 else 'disabled (historical suite).'), '',
             'Candidate stencil-state publication: ' + ('enabled.' if suite.get('managed_stencil_state', False)
                                                       else 'disabled (historical suite).'), '',
             'D3D12, 1280x720, full rendering, audio on, fixed-step Time Attack, uncapped.',
             'Executable, save copies, caches and logs were staged on the local disk before measurement.',
             'Game data already on a local fixed disk is reused read-only; network game data is copied locally first.',
             'Per-draw timers are off. Overall present intervals do not separate CPU and GPU work.', '',
             f"Every number below uses race-object-present indices {suite['skip']}..{suite['window_end']-1} "
             f"({suite['window_end']-suite['skip']} intervals).",
             'WARNING: racing=1 includes loading/preparation; equal record indices do NOT prove equal gameplay progress.',
             'Data integrity can pass while scene alignment is unverified. These ratios are mixed-window observations, not verified gameplay speedups.',
             'Percentiles use nearest rank. Verification is excluded from all performance numbers.',
             'The >16.67 ms count is an uncapped interval diagnostic, not measured display-refresh misses.', '',
             '| Stage | Setting | Round | FPS | Mean ms | P95 ms | P99 ms | >16.67 ms | >=33.33 ms | >50 ms |',
             '| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
    for run in runs:
        if 'fps' in run:
            lines.append('| {stage} | {config} | {repeat} | {fps:.2f} | {mean_ms:.3f} | {p95_ms:.3f} | {p99_ms:.3f} | {over_16_67} | {over_33_33} | {over_50} |'.format(**run))
    lines += ['', '## Paired FPS ratios', '',
              'A ratio above 1 means the second setting completed its nominal record window faster; workload equivalence is unverified.',
              'Compare effects with the control spread and the individual rounds; small changes within noise are inconclusive.', '']
    for stage, first, second in [('control', 'codex-before', 'codex-control'),
                                 ('compare', 'codex-before', 'codex-after'),
                                 ('compare', 'codex-after', 'managed-buffers'),
                                 ('compare', 'codex-before', 'managed-buffers')]:
        by_key = {(r['config'], r['repeat']): r for r in runs if r['stage'] == stage and 'fps' in r}
        ratios = []
        for repeat in range(1, (suite['control_repeats'] if stage == 'control' else suite['repeats']) + 1):
            a, b = by_key.get((first, repeat)), by_key.get((second, repeat))
            if a and b:
                ratios.append(b['fps'] / a['fps'])
        if ratios:
            lines.append(f"- {stage}, {second} / {first}: " + ', '.join(f'{v:.4f}' for v in ratios) +
                         f'; median {statistics.median(ratios):.4f}.')
    lines += ['', '## Completion and failures', '']
    if suite.get('error'):
        lines.append('Suite error: ' + suite['error'])
    for run in runs:
        label = f"{run['stage']}/{run['config']}-{run['repeat']}"
        lines.append(f"- {label}: " + ('; '.join(run['errors']) if run['errors'] else
                     f"{suite['race_frames']} race-object presents, expected stop, metadata checked; gameplay alignment unverified."))
    lines += ['', '## Supporting evidence', '',
              '- [Verification screenshots and logs](verify/)',
              '- [Control hardware/settings and full-run summary](control/summary.md)',
              '- [Comparison hardware/settings and full-run summary](compare/summary.md)',
              '- [Suite status, paths and hashes](suite.json)',
              '- [Machine-readable per-run measurements](measurements.json)', '',
              'This is a fixed Time Attack segment. Free Race with opponents, three tracks, scene reloads,',
              'a completed race and 30 minutes of displayed 60 Hz gameplay remain separate acceptance tests.',
              'The two control pairs are a preliminary noise check; expand them if the candidate effect is small.',
              'Fewer than six comparison rounds is exploratory only.', '']
    (directory / 'measurements.json').write_text(json.dumps(dict(valid=valid,
        window_basis='legacy_race_object_present', gameplay_alignment_verified=False,
        runs=runs), indent=2) + '\n', encoding='utf-8')
    (directory / 'report.md').write_text('\n'.join(lines), encoding='utf-8')
    return valid


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    ok = build_report(args.directory)
    print(f"i5 report: {args.directory / 'report.md'} ({'complete' if ok else 'incomplete/failed'})")
    raise SystemExit(0 if ok else 1)
