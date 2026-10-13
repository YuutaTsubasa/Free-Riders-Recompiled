"""Audit completed, local i5 result copies without rereading or changing frame logs.

Integrity, recorded context, and displayed-60-Hz acceptance are separate things.
This audit neither excludes rounds nor infers actual AC power from battery labels.
"""
import argparse
import json
import re
from pathlib import Path


CONFIGS = {'verify': ('verify-buffers',), 'control': ('codex-before', 'codex-control'),
           'compare': ('codex-before', 'codex-after', 'managed-buffers')}


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def audit_quality(directory):
    directory = Path(directory)
    result = dict(integrity_valid=False, context={}, observations=[], findings=[])
    findings = result['findings']
    counts = {}

    def finding(code, level, detail, **scope):
        findings.append(dict(code=code, level=level, detail=detail, **scope))

    try:
        suite, measurements = read_json(directory / 'suite.json'), read_json(directory / 'measurements.json')
        counts = {'verify': 1, 'control': suite['control_repeats'], 'compare': suite['repeats']}
        if any(type(n) is not int or not 1 <= n <= 100 for n in counts.values()):
            raise ValueError('Invalid configured round counts')
        runs = measurements['runs']
        if not isinstance(runs, list):
            raise ValueError('Measurements runs must be a list')
        expected = {(stage, config, n) for stage, configs in CONFIGS.items()
                    for config in configs for n in range(1, counts[stage] + 1)}
        actual = set()
        for run in runs:
            key = (run['stage'], run['config'], run['repeat'])
            if any(type(v) is not str for v in key[:2]) or type(key[2]) is not int:
                raise ValueError('Invalid run identity')
            if key in actual:
                finding('duplicate_run', 'error', str(key))
            actual.add(key)
            if run.get('errors') != []:
                finding('run_errors', 'error', str(key))
        if actual != expected:
            finding('run_set_mismatch', 'error',
                    f'Missing: {sorted(expected - actual)}; unexpected: {sorted(actual - expected)}')
        if suite.get('status') != 'completed' or measurements.get('valid') is not True:
            finding('incomplete_integrity_report', 'error', 'Suite/measurements do not record successful completion')
        result['integrity_valid'] = not any(f['level'] == 'error' for f in findings)
        if counts['compare'] < 6:
            finding('exploratory_round_count', 'caution', 'Fewer than six comparison rounds')
        if counts['control'] < 2:
            finding('insufficient_control_pairs', 'caution', 'Fewer than two preliminary control pairs')
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
        finding('invalid_integrity_input', 'error', str(error))

    for stage in ('control', 'compare'):
        path = directory / stage / 'info.txt'
        source = f'{stage}/info.txt'
        try:
            lines = path.read_text(encoding='utf-8-sig', errors='replace').splitlines()
            raw = [line for line in lines if line.startswith('power=')]
            samples = [m for line in raw if (m := re.fullmatch(
                r'power=(.*?) plan=(.*?) idle_cpu_percent=([0-9]+(?:\.[0-9]+)?)', line))]
            if len(samples) != 1 or not 0 <= float(samples[0][3]) <= 100:
                raise ValueError('Missing, ambiguous or invalid stage-admission CPU context')
            power, plan, cpu = samples[0].groups()
            result['context'][stage] = dict(source=source, raw_power_lines=raw,
                                            power_label=power, plan_label=plan, admission_cpu_percent=float(cpu))
            if any(not label.strip() or label.strip().lower() == 'unknown' for label in (power, plan)):
                finding('unknown_power_context', 'unknown', 'Power or plan label was not established',
                        stage=stage, source=source)
            if float(cpu) > 15:  # Same threshold as benchmark.ps1's existing warning.
                finding('admission_cpu_load', 'caution',
                        f'{cpu}% before this stage; not a measurement of load throughout its runs', stage=stage, source=source)
            if power == 'ON BATTERY':
                finding('unverified_power_label', 'caution',
                        'Win32_Battery label only; actual AC state, desktop/UPS behavior and throttling are unverified',
                        stage=stage, source=source)
        except (OSError, ValueError) as error:
            result['context'][stage] = dict(source=source, status='unknown')
            finding('missing_context', 'unknown', str(error), stage=stage, source=source)

    notes_path = directory / 'measurement-notes.json'
    try:
        notes = read_json(notes_path)['observations']
        if not isinstance(notes, list):
            raise ValueError('observations must be a list')
        for note in notes:
            if not isinstance(note, dict) or not isinstance(note.get('detail'), str) or not note['detail'].strip():
                raise ValueError('Each observation needs a nonempty detail')
            stage, rounds = note.get('stage', 'suite'), note.get('rounds', [])
            if stage not in (*CONFIGS, 'suite') or not isinstance(rounds, list) or any(
                    type(n) is not int or n < 1 for n in rounds):
                raise ValueError('Invalid observation scope')
            if rounds and (stage == 'suite' or stage not in counts or any(n > counts[stage] for n in rounds)):
                raise ValueError('Observation rounds are outside the configured stage')
            result['observations'].append(dict(stage=stage, rounds=rounds, detail=note['detail']))
            finding('operator_observation', 'caution', note['detail'], stage=stage, rounds=rounds,
                    source='measurement-notes.json')
        result['observer_record'] = 'provided; no recorded observations' if not notes else 'provided'
    except (OSError, ValueError, KeyError, TypeError) as error:
        result['observer_record'] = 'unknown'
        finding('missing_observer_record', 'unknown', str(error), source='measurement-notes.json')

    result['quality_status'] = ('incomplete' if not result['integrity_valid'] else
                                'review_required' if any(f['level'] == 'caution' for f in findings) else
                                'unknown' if findings else 'no_recorded_context_issue')
    result['requires_review'] = result['quality_status'] != 'no_recorded_context_issue'
    return result


def write_audit(directory):
    directory = Path(directory)
    result = audit_quality(directory)
    (directory / 'quality.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    lines = ['# i5 measurement context audit', '',
             f"Data integrity: {'complete' if result['integrity_valid'] else 'incomplete/failed'}.", '',
             f"Recorded context: {result['quality_status']}.", '',
             'This audit does not modify FPS, original valid flags, ratios or round inclusion.',
             'Stage-admission CPU is the runner\'s rounded five-sample pre-stage average, not per-run background load.',
             'Battery/power labels are heuristics; they do not establish actual AC state or CPU/GPU throttling.', '',
             '## Findings', '']
    for f in result['findings']:
        scope = f.get('stage', 'suite') + (f" rounds {f['rounds']}" if f.get('rounds') else '')
        lines.append(f"- {f['level']} / {scope} / {f['code']}: " + f['detail'].replace('\n', ' '))
    if not result['findings']:
        lines.append('- No issue in the provided context records; this does not prove an otherwise idle machine.')
    lines += ['', 'Preserve all rounds. Any exclusion needs a stated reason and an all-round sensitivity comparison.',
              'Even complete data with no recorded context issue is NOT displayed stable-60-Hz acceptance.', '']
    (directory / 'quality.md').write_text('\n'.join(lines), encoding='utf-8')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path, help='Completed local result copy; never an active target suite')
    result = write_audit(parser.parse_args().directory)
    print(f"i5 context audit: {result['quality_status']} (original performance data unchanged)")
    raise SystemExit(0 if result['integrity_valid'] else 1)
