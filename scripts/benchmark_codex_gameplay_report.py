"""Strict, offline gameplay_v1 suite integrity and uncapped interval statistics."""
import argparse
import copy
import json
import re
import statistics
from pathlib import Path

from benchmark_gameplay import inspect_gameplay
from benchmark_summary import present_record


ROUTE = 'ok@580,ok@930,ok@1280,start@1630,ok@1830,right@2030,ok@2230,ok@2530,ok@2830,right@3130,ok@3430'
GAME_CONFIG = [1, 2, 1, 0, 3, 60, 0]
SEGMENTS = ('countdown', 'racing', 'combined', 'race_window')
EXPECTED_SETTINGS = dict(
    SFR_MANAGED_CONSTANTS='1', SFR_MANAGED_TEXTURE_STATE='1', SFR_MANAGED_STENCIL_STATE='1',
    SFR_MANAGED_BUFFERS='0', SFR_DIRECT_CONSTANT_UPLOAD='0', SFR_REALTIME_RACE='0',
    SFR_REALTIME_UI='1', SFR_SAY_REFERENCE_FPS='40', SFR_SAY_MIN_SECONDS='2',
    SFR_RENDER_EVERY='1', SFR_ALLOW_RENDER_TARGETS='1', SFR_AUDIO='1', SFR_FRAME_METRICS='1',
    SFR_GPU_PIPELINE='1', SFR_VERTEX_CACHE='1',
    SFR_DRAW_TIMERS='0', SFR_GRAPHICS='d3d12', SFR_WINDOW_WIDTH='1280', SFR_WINDOW_HEIGHT='720',
    SFR_RENDER_SCALE='100', SFR_ASPECT_MODE='original', SFR_UI_ALIGN='edge',
    SFR_VSYNC='0', SFR_FULLSCREEN='0', SFR_FRAME_LIMIT='0', SFR_PRESENT_LIMIT='0',
    SFR_PRESENT_LIMIT_RACING='0', SFR_PRESENT_LIMIT_AFTER_SAY='0',
    SFR_GAMEPLAY_METRICS='1', SFR_GAMEPLAY_TICK_LIMIT='5400', SFR_PROFILE='1',
    SFR_PLAYER1_INPUT='keyboard', SFR_PLAYER2_INPUT='off', SFR_NUI_HAND_CENTRED='1', SFR_SAY=ROUTE)
VERIFY_SETTINGS = ('SFR_VERIFY_MANAGED_CONSTANTS', 'SFR_VERIFY_MANAGED_TEXTURE_STATE',
                   'SFR_VERIFY_MANAGED_STENCIL_STATE', 'SFR_D3D12_DRED')
DISABLED_SETTINGS = ('SFR_SKIP_DRAWS', 'SFR_SKIP_PRESENT', 'SFR_RACE_PROGRESS',
                     'SFR_SKIP_FOREIGN_TARGETS', 'SFR_DRAW_DUMP', 'SFR_DRAW_DUMP_SOURCE',
                     'SFR_TEXTURE_STATS', 'SFR_TEXTURE_SURVEY', 'SFR_PIPELINE_KEY_VERIFY',
                     'SFR_VERIFY_MANAGED_BUFFERS', 'SFR_DIAGNOSTIC_ENTRIES',
                     'SFR_MAIN_PROFILE', 'SFR_PROFILE_GUEST', 'SFR_PROFILE_AFTER',
                     'SFR_HOST_PROFILE', 'SFR_SAMPLE_PROFILE',
                     'SFR_BUFFER_AUDIT', 'SFR_TEXTURE_TIMERS')


def stage_plan(suite):
    return [('verify', ['semantic-verify'], 1),
            ('control', ['semantic-before', 'semantic-control'], suite['control_repeats']),
            ('compare', ['semantic-before', 'semantic-after'], suite['repeats'])]


def fixture_hashes(records):
    """PowerShell serializes a single fixture as an object, multiple as a list."""
    if isinstance(records, dict):
        records = [records]
    if not isinstance(records, list) or not records:
        raise ValueError('Missing or invalid fixture hash records')
    result = {}
    for record in records:
        if not isinstance(record, dict) or not isinstance(record.get('path'), str):
            raise ValueError('Invalid fixture path record')
        path = record['path'].replace('\\', '/').lstrip('/')
        sha = record.get('sha256')
        if not path or ':' in path or any(part in ('', '.', '..') for part in path.split('/')):
            raise ValueError('Invalid fixture relative path')
        if path.casefold() in {existing.casefold() for existing in result}:
            raise ValueError('Duplicate fixture path')
        if not isinstance(sha, str) or not re.fullmatch('[0-9a-fA-F]{64}', sha):
            raise ValueError('Invalid fixture SHA-256')
        result[path] = sha.lower()
    return result


def validate_suite(suite):
    """Refuse legacy manifests before reading any legacy logs or writing outputs."""
    errors = []
    if not isinstance(suite, dict) or suite.get('protocol') != 'gameplay_v1':
        return ['Unsupported protocol: explicit gameplay_v1 suite required; legacy protocol is not consumed']
    for key, expected in [('gameplay_ticks', 5400), ('skip', 1200), ('window_end', 4500),
                          ('expected_game_config', GAME_CONFIG), ('route_say', ROUTE)]:
        if suite.get(key) != expected:
            errors.append(f'Unexpected suite {key}: require {expected!r}')
        elif type(expected) is int and type(suite[key]) is not int:
            errors.append(f'Invalid suite {key} type: require integer')
        elif key == 'expected_game_config' and any(type(v) is not int for v in suite[key]):
            errors.append('Invalid suite expected_game_config type: require integers')
    for key, minimum in [('repeats', 6), ('control_repeats', 2)]:
        count = suite.get(key)
        if type(count) is not int or not minimum <= count <= 100:
            errors.append(f'Unexpected suite {key}: require {minimum}..100')
    for key in ('baseline_sha256', 'candidate_sha256'):
        if not isinstance(suite.get(key), str) or not re.fullmatch('[0-9a-fA-F]{64}', suite[key]):
            errors.append(f'Invalid suite {key}')
    try:
        fixture_hashes(suite.get('fixture_files'))
    except ValueError as error:
        errors.append('Suite fixture_files: ' + str(error))
    if suite.get('status') != 'completed':
        errors.append('Suite status is not completed')
    if all(type(suite.get(key)) is int and 1 <= suite[key] <= 100
           for key in ('repeats', 'control_repeats')):
        expected_stages = stage_plan(suite)
        stages = suite.get('stages')
        if not isinstance(stages, list) or len(stages) != 3:
            errors.append('Missing or invalid suite stages')
        else:
            for entry, (name, configs, count) in zip(stages, expected_stages):
                if (not isinstance(entry, dict) or entry.get('name') != name or
                    entry.get('configs') != configs or entry.get('repeats') != count or
                    entry.get('status') not in ('passed', 'completed')):
                    errors.append(f'Incomplete or changed stage: {name}')
    return errors


def inspect_run(directory, config, repeat, suite):
    directory = Path(directory)
    result = dict(stage=directory.name, config=config, repeat=repeat, valid=False, errors=[])
    errors = result['errors']
    label = f'{config}-{repeat}'
    log, metadata = directory / (label + '.log'), directory / (label + '.json')
    if not log.is_file() or not metadata.is_file():
        errors.append('Missing log or executable/environment metadata')
        return result
    try:
        meta = json.loads(metadata.read_text(encoding='utf-8-sig'))
        if not isinstance(meta, dict) or not isinstance(meta.get('settings'), dict):
            raise ValueError('Invalid executable/environment metadata schema')
        result['exit_code'] = meta.get('exit_code')
        if type(result['exit_code']) is not int or result['exit_code'] != 3:
            errors.append('Expected process exit_code=3 after gameplay-limit RuntimeStop')
        if 'admission' in meta:
            result['admission'] = meta['admission']
        old = config in ('semantic-before', 'semantic-control')
        expected_hash = suite['baseline_sha256' if old else 'candidate_sha256']
        result['sha256'] = meta.get('sha256')
        if not isinstance(result['sha256'], str) or result['sha256'].lower() != expected_hash.lower():
            errors.append('Executable hash differs from suite manifest')
        settings = meta['settings']
        for key, value in EXPECTED_SETTINGS.items():
            if settings.get(key) != value:
                errors.append(f'Unexpected {key}: {settings.get(key)!r}; require {value!r}')
        verify = config == 'semantic-verify'
        for key in VERIFY_SETTINGS:
            if settings.get(key, '0') != ('1' if verify else '0'):
                errors.append(f'Unexpected {key}')
        # The old executable always times texture lookup; absence of its opt-in
        # marker is intentional. New executables must explicitly disable it.
        result['texture_timing'] = observed_texture_timing(log)
        if not old and result['texture_timing'] != 'disabled':
            errors.append('Candidate present marker does not confirm texture timers disabled')
        for key in DISABLED_SETTINGS:
            if key == 'SFR_TEXTURE_TIMERS':
                value = settings.get(key, '0' if old else None)
            else:
                value = settings.get(key, '0')
            if value not in ('0', ''):
                errors.append(f'Diagnostic setting present: {key}')
        for key, value in settings.items():
            if not isinstance(key, str) or not isinstance(value, str):
                errors.append('Settings must contain string keys and values')
                continue
            diagnostic = (any(word in key for word in ('TRACE', 'PROBE', 'STALL', 'SFR_UW_')) or
                          key.startswith('SFR_SCREENSHOT'))
            if verify and key.startswith('SFR_SCREENSHOT'):
                continue
            if diagnostic and value not in ('0', ''):
                errors.append(f'Diagnostic setting present: {key}')
        progress = dict(inspect_gameplay(log, suite['skip'], suite['window_end'], suite['gameplay_ticks']))
        if list(progress['config']) != suite['expected_game_config']:
            errors.append('Observed game configuration differs from expected_game_config')
        progress['observed_game_config'] = progress.pop('config')
        result.update(progress)
    except (ValueError, OSError, KeyError, TypeError, OverflowError) as error:
        errors.append(str(error))
    result['valid'] = not errors
    return result


def observed_texture_timing(log):
    """Use the actual present marker, including the historical missing=on rule."""
    with Path(log).open(encoding='utf-8-sig', errors='replace') as stream:
        for raw in stream:
            line = present_record(raw)
            if line:
                row = dict(token.split('=', 1) for token in line.split()[1:] if '=' in token)
                marker = row.get('texture_timers')
                if marker is None:
                    return 'enabled (legacy marker absent)'
                if marker not in ('0', '1'):
                    raise ValueError('Invalid observed texture_timers marker')
                return 'enabled' if marker == '1' else 'disabled'
    return 'unknown (no present record)'


def summarize(runs, suite):
    pairs, thresholds = {}, {}
    valid = [run for run in runs if run['valid'] and run['stage'] != 'verify']
    for stage, second in [('control', 'semantic-control'), ('compare', 'semantic-after')]:
        by_key = {(r['config'], r['repeat']): r for r in valid if r['stage'] == stage}
        count = suite['control_repeats' if stage == 'control' else 'repeats']
        pairs[stage] = {}
        for segment in SEGMENTS:
            ratios = [by_key[(second, n)][segment]['fps'] / by_key[('semantic-before', n)][segment]['fps']
                      for n in range(1, count + 1)
                      if (second, n) in by_key and ('semantic-before', n) in by_key]
            pairs[stage][segment] = dict(paired_fps_ratios=ratios, pairs=len(ratios),
                complete=len(ratios) == count,
                median_ratio=statistics.median(ratios) if ratios else None,
                min_ratio=min(ratios) if ratios else None, max_ratio=max(ratios) if ratios else None,
                max_absolute_deviation_from_one=max((abs(r - 1) for r in ratios), default=None))
    for stage, configs, count in stage_plan(suite)[1:]:
        for config in configs:
            selected = [r for r in valid if r['stage'] == stage and r['config'] == config]
            for segment in SEGMENTS[:3]:
                worst95 = max((r[segment]['p95_ms'] for r in selected), default=None)
                worst99 = max((r[segment]['p99_ms'] for r in selected), default=None)
                thresholds[f'{stage}/{config}/{segment}'] = dict(
                    replicates=len(selected), expected_replicates=count,
                    worst_replicate_p95_ms=worst95, worst_replicate_p99_ms=worst99,
                    statistical_target_met=(len(selected) == count and worst95 <= 15 and worst99 <= 1000 / 60))
    return pairs, thresholds


def quiet_context(directory, suite=None):
    """Recorded pre-stage load is separate from integrity and all-round ratios."""
    context = {}
    for stage in ('control', 'compare'):
        entry = dict(source=f'{stage}/info.txt', status='unknown',
                     quiet_throughout_runs_verified=False)
        try:
            matches = []
            with (Path(directory) / stage / 'info.txt').open(encoding='utf-8-sig', errors='replace') as stream:
                for line in stream:
                    match = re.fullmatch(r'power=(.*?) plan=(.*?) idle_cpu_percent=([0-9]+(?:\.[0-9]+)?)\s*', line)
                    if match:
                        matches.append(match.groups())
            if len(matches) == 1 and 0 <= float(matches[0][2]) <= 100:
                power, plan, cpu = matches[0]
                entry.update(status='recorded_pre_stage_only' if float(cpu) <= 15 else 'review_required',
                             admission_cpu_percent=float(cpu), power_label=power, plan_label=plan)
        except OSError:
            pass
        preflight = (suite or {}).get('preflight', [])
        matching = [item for item in preflight if isinstance(item, dict) and item.get('stage') == stage] if isinstance(preflight, list) else []
        if len(matching) == 1:
            entry['startup_admission'] = matching[0]
        context[stage] = entry
    return context


def inspect_suite(directory):
    directory = Path(directory)
    result = dict(protocol='gameplay_v1', valid=False, errors=[], runs=[], paired_ratios={},
                  statistical_thresholds={}, displayed_60hz_verified=False,
                  input_latency_verified=False, heavy_free_race_verified=False,
                  quiet_context={})
    try:
        suite = json.loads((directory / 'suite.json').read_text(encoding='utf-8-sig'))
        if isinstance(suite, dict) and suite.get('resumed_from'):
            result['resumed_from'] = suite['resumed_from']
            for key in ('source_suite', 'resume_source_suite', 'import_inventory'):
                if key in suite:
                    result[key] = suite[key]
        result['errors'] = validate_suite(suite)
        if result['errors']:
            return result
        result['source_commits'] = dict(baseline=suite.get('baseline_commit') or 'unknown',
            candidate_runtime=suite.get('candidate_runtime_commit') or 'unknown',
            protocol_packaging=suite.get('candidate_commit') or 'unknown')
        result['quiet_context'] = quiet_context(directory, suite)
        expected_fixture = fixture_hashes(suite['fixture_files'])
        for stage, configs, count in stage_plan(suite):
            try:
                recorded_fixture = json.loads((directory / stage / 'fixture-hashes.json').read_text(encoding='utf-8-sig'))
                if fixture_hashes(recorded_fixture) != expected_fixture:
                    raise ValueError('Save fixture paths/hashes differ from suite fixture_files')
            except (OSError, ValueError, TypeError) as error:
                result['errors'].append(f'{stage}/fixture-hashes.json: {error}')
            for repeat in range(1, count + 1):
                for config in configs:
                    result['runs'].append(inspect_run(directory / stage, config, repeat, suite))
        result['paired_ratios'], result['statistical_thresholds'] = summarize(result['runs'], suite)
        result['valid'] = not result['errors'] and all(r['valid'] for r in result['runs'])
        if not result['valid']:
            result['errors'].append('One or more required runs failed integrity validation')
    except (ValueError, OSError, KeyError, TypeError, OverflowError) as error:
        result['errors'].append(str(error))
    return result


def inspect_resume(directory):
    """Read-only gate for the one failed boundary before comparison starts.

    Hypothetical completion validates the header schema only. Actual retained
    logs, metadata and save fixtures must independently pass every integrity
    check; no hypothetical comparison measurements are created or returned.
    """
    directory = Path(directory)
    result = dict(resume_valid=False, errors=[])
    errors = result['errors']
    try:
        suite = json.loads((directory / 'suite.json').read_text(encoding='utf-8-sig'))
        if not isinstance(suite, dict) or suite.get('protocol') != 'gameplay_v1':
            raise ValueError('Resume requires explicit gameplay_v1 protocol')
        if suite.get('status') != 'failed':
            errors.append('Resume requires failed suite status before compare')
        if type(suite.get('repeats')) is not int or suite['repeats'] != 6:
            errors.append('Resume requires exactly six comparison repeats')
        if type(suite.get('control_repeats')) is not int or suite['control_repeats'] != 2:
            errors.append('Resume requires exactly two control pairs')
        stages = suite.get('stages')
        if not isinstance(stages, list) or len(stages) != 2:
            errors.append('Resume requires only passed verify and control stages')
        else:
            expected = [('verify', ['semantic-verify'], 1),
                        ('control', ['semantic-before', 'semantic-control'], 2)]
            for stage, (name, configs, count) in zip(stages, expected):
                if (not isinstance(stage, dict) or stage.get('name') != name or
                    stage.get('configs') != configs or type(stage.get('repeats')) is not int or
                    stage['repeats'] != count or stage.get('status') != 'passed'):
                    errors.append(f'Resume requires exact passed {name} stage')
        if (directory / 'compare').exists() or (directory / 'compare').is_symlink():
            errors.append('Resume refuses any existing compare path')
        if errors:
            return result
        hypothetical = copy.deepcopy(suite)
        hypothetical['status'] = 'completed'
        hypothetical['stages'].append(dict(name='compare', configs=['semantic-before', 'semantic-after'],
                                           repeats=6, status='passed'))
        errors.extend(validate_suite(hypothetical))
        if errors:
            return result
        expected_fixture = fixture_hashes(suite['fixture_files'])
        for stage, configs, count in stage_plan(suite)[:2]:
            try:
                recorded = json.loads((directory / stage / 'fixture-hashes.json').read_text(encoding='utf-8-sig'))
                if fixture_hashes(recorded) != expected_fixture:
                    raise ValueError('Save fixture paths/hashes differ from suite fixture_files')
            except (OSError, ValueError, TypeError) as error:
                errors.append(f'{stage}/fixture-hashes.json: {error}')
            for repeat in range(1, count + 1):
                for config in configs:
                    run = inspect_run(directory / stage, config, repeat, suite)
                    errors.extend(f'{stage}/{config}-{repeat}: {error}' for error in run['errors'])
        if errors:
            return result
        keys = ('baseline_sha256', 'candidate_sha256', 'expected_game_config', 'gameplay_ticks',
                'skip', 'window_end', 'repeats', 'control_repeats', 'fixture_files', 'route_say',
                'baseline_commit', 'candidate_runtime_commit', 'candidate_commit')
        result.update(resume_valid=True,
                      source_params={key: suite.get(key) for key in keys},
                      stages=copy.deepcopy(stages), preflight=copy.deepcopy(suite.get('preflight', [])),
                      source_timestamps={key: suite.get(key) for key in ('started', 'completed')},
                      source_suite=suite)
    except (ValueError, OSError, KeyError, TypeError, OverflowError) as error:
        errors.append(str(error))
    return result


def build_report(directory):
    directory = Path(directory)
    result = inspect_suite(directory)
    # Never overwrite historical reports, including when pointed at old data.
    lines = ['# Semantic gameplay suite', '',
             'Integrity: ' + ('VALID' if result['valid'] else 'INVALID / INCOMPLETE'), '',
             'Fixed-step Time Attack, full rendering/audio, uncapped D3D12 at 1280x720.',
             'All 300 countdown and 5100 racing ticks are checked, including the final countdown transition.',
             'The race window is racing ticks 1201..4500 (3300 intervals). Percentiles use nearest rank.',
             'Verification runs are excluded from ratios and statistical targets.',
             'Scope: the subsequent batch comparison with managed features on in both versions; this does not estimate total R1-R4 benefit.',
             'Texture timing is observed from the first present record in each run; a missing historical marker means enabled.',
             'Present intervals do not separate CPU and GPU cost.', '',
             'Recorded launch-admission snapshots are preserved per run; they do not prove continuous quiet execution.',
             'Displayed stable 60 Hz, input latency, and heavy Free Race are NOT VERIFIED.',
             'Statistical targets only: P95 <=15 ms and P99 <=16.666667 ms for countdown, racing and combined.',
             'Each target uses the worst replicate percentile; a median cannot hide a failing tail.', '',
             '| Stage/config | Round | Segment | Intervals | Mean ms | P95 ms | P99 ms | Max ms |',
             '| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: |']
    commits = result.get('source_commits', {})
    lines[3:3] = [f"Baseline source: `{commits.get('baseline', 'unknown')}`; "
                  f"candidate runtime source: `{commits.get('candidate_runtime', 'unknown')}`; "
                  f"protocol packaging source: `{commits.get('protocol_packaging', 'unknown')}`.", '']
    if result.get('resumed_from'):
        lines[3:3] = [f"Resumed from: `{result['resumed_from']}`.",
                      'Original verification and controls were reused across an interruption. '
                      'Their original times, admission and context remain part of the evidence; '
                      'the earlier control spread does not establish noise during the later comparison.', '']
    for run in result['runs']:
        if not run['valid'] or run['stage'] == 'verify':
            continue
        for segment in SEGMENTS:
            m = run[segment]
            lines.append(f"| {run['stage']}/{run['config']} | {run['repeat']} | {segment} | {m['frames']} | "
                         f"{m['mean_ms']:.3f} | {m['p95_ms']:.3f} | {m['p99_ms']:.3f} | {m['max_ms']:.3f} |")
    lines += ['', '## Paired ratios and control noise', '',
              'Ratios are second/before FPS. Compare effects with control spread; small effects within noise are inconclusive.', '']
    for stage, segments in result['paired_ratios'].items():
        for segment, stats in segments.items():
            if stats['pairs']:
                lines.append(f"- {stage}/{segment}: median {stats['median_ratio']:.4f}; "
                             f"range {stats['min_ratio']:.4f}..{stats['max_ratio']:.4f}; "
                             f"largest deviation from 1 {stats['max_absolute_deviation_from_one']:.4f}; "
                             f"{stats['pairs']} pairs ({'complete' if stats['complete'] else 'INCOMPLETE'}).")
    lines += ['', '## Statistical targets by worst replicate', '']
    for label, stats in result['statistical_thresholds'].items():
        if stats['replicates']:
            lines.append(f"- {label}: {'MET' if stats['statistical_target_met'] else 'NOT MET'}; "
                         f"worst P95 {stats['worst_replicate_p95_ms']:.3f} ms, "
                         f"worst P99 {stats['worst_replicate_p99_ms']:.3f} ms; "
                         f"{stats['replicates']}/{stats['expected_replicates']} replicates.")
        else:
            lines.append(f'- {label}: NOT MET; no valid replicates.')
    lines += ['', '## Recorded quiet-run context', '',
              'Pre-stage CPU samples do not establish load throughout individual runs. '
              'Context does not exclude rounds or change integrity/statistical results.', '']
    for stage, context in result['quiet_context'].items():
        load = context.get('admission_cpu_percent')
        lines.append(f"- {stage}: {context['status']}; " +
                     (f'pre-stage CPU {load:g}%.' if load is not None else 'pre-stage CPU unknown.'))
        admission = context.get('startup_admission')
        if admission:
            lines.append(f"- {stage} startup admission: CPU {admission.get('cpu_percent', 'unknown')}%; "
                         f"existing game processes {admission.get('game_processes', 'unknown')}; "
                         f"admitted={admission.get('admitted', 'unknown')}. This is not continuous CPU proof.")
    lines += ['', '## Integrity errors', '']
    lines += ['- ' + error for error in result['errors']]
    for run in result['runs']:
        if run['errors']:
            lines.append(f"- {run['stage']}/{run['config']}-{run['repeat']}: " + '; '.join(run['errors']))
    lines += ['', '## Observed texture instrumentation', '']
    for stage, config in sorted({(r['stage'], r['config']) for r in result['runs']}):
        observed = sorted({r.get('texture_timing', 'unknown') for r in result['runs']
                           if r['stage'] == stage and r['config'] == config})
        lines.append(f"- {stage}/{config}: " + ', '.join(observed) + '.')
    if result['valid']:
        lines.append('- None. Required hashes, settings, route, progress, configurations and stops passed.')
    (directory / 'gameplay-measurements.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    (directory / 'gameplay-report.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    return result['valid']


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path, nargs='?')
    parser.add_argument('--resume-check', type=Path, metavar='DIRECTORY',
                        help='read-only JSON validation of passed verify/control before compare')
    args = parser.parse_args()
    if args.resume_check:
        if args.directory:
            parser.error('Use --resume-check DIRECTORY without a report directory')
        result = inspect_resume(args.resume_check)
        print(json.dumps(result, indent=2))
        raise SystemExit(0 if result['resume_valid'] else 1)
    if args.directory is None:
        parser.error('A report directory or --resume-check DIRECTORY is required')
    ok = build_report(args.directory)
    print(f"Gameplay report: {args.directory / 'gameplay-report.md'} ({'valid' if ok else 'invalid/incomplete'})")
    raise SystemExit(0 if ok else 1)
