"""i5 suite integrity, result validity and safe local staging without game assets."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from benchmark_codex_i5_report import inspect_run, build_report


def suite(managed_texture_state=True, managed_stencil_state=True):
    state = dict(candidate_sha256='C' * 64, baseline_sha256='B' * 64, race_frames=5400,
                 skip=1200, window_end=4500, repeats=1, control_repeats=1,
                 candidate_commit='test-only', status='completed')
    if managed_texture_state:
        state['managed_texture_state'] = True
    if managed_stencil_state:
        state['managed_stencil_state'] = True
    return state


def write_run(path, config, count=5400, stop=True, managed_texture_state=True, managed_stencil_state=True):
    path.mkdir(parents=True, exist_ok=True)
    old = config in ('codex-before', 'codex-control')
    settings = dict(SFR_DRAW_TIMERS='0', SFR_FRAME_METRICS='1', SFR_REALTIME_RACE='0',
                    SFR_RENDER_EVERY='1', SFR_AUDIO='1', SFR_FRAME_LIMIT='0', SFR_GRAPHICS='d3d12',
                    SFR_WINDOW_WIDTH='1280', SFR_WINDOW_HEIGHT='720', SFR_PRESENT_LIMIT_RACING='5400')
    if not old:
        settings['SFR_MANAGED_CONSTANTS'] = '1'
        if managed_stencil_state:
            settings['SFR_MANAGED_STENCIL_STATE'] = '1'
        if managed_texture_state:
            settings['SFR_MANAGED_TEXTURE_STATE'] = '1'
    if config in ('managed-buffers', 'verify-buffers'):
        settings['SFR_MANAGED_BUFFERS'] = '1'
    if config == 'verify-buffers':
        settings.update(SFR_VERIFY_MANAGED_BUFFERS='1', SFR_D3D12_DRED='1')
        if managed_stencil_state:
            settings['SFR_VERIFY_MANAGED_STENCIL_STATE'] = '1'
        if managed_texture_state:
            settings.update(SFR_VERIFY_MANAGED_CONSTANTS='1', SFR_VERIFY_MANAGED_TEXTURE_STATE='1')
    (path / f'{config}-1.json').write_text(json.dumps(dict(settings=settings, sha256=('B' if old else 'C') * 64)))
    seconds = 0.0
    lines = ['NATIVE_PRESENT frame=0 racing=0 seconds=0']
    for i in range(count):
        seconds += .01 if i < 1200 else (.02 if i < 4500 else .1)
        lines.append(f'NATIVE_PRESENT frame={i+1} racing=1 seconds={seconds:.6f}')
    if stop:
        lines.append('STOP present-limit @0x0: test')
    (path / f'{config}-1.log').write_text('\n'.join(lines) + '\n')


class I5ResultsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / 'compare'

    def change_settings(self, config, **changes):
        meta_file = self.path / f'{config}-1.json'
        meta = json.loads(meta_file.read_text())
        for key, value in changes.items():
            if value is None:
                meta['settings'].pop(key, None)
            else:
                meta['settings'][key] = value
        meta_file.write_text(json.dumps(meta))

    def test_percentiles_and_fps_use_identical_window(self):
        write_run(self.path, 'managed-buffers')
        result = inspect_run(self.path, 'managed-buffers', 1, suite())
        self.assertEqual(result['errors'], [])
        self.assertEqual(result['frames'], 3300)
        self.assertAlmostEqual(result['fps'], 50)
        self.assertAlmostEqual(result['p99_ms'], 20)
        self.assertEqual(result['over_33_33'], 0)

    def test_expected_stop_does_not_hide_short_run(self):
        write_run(self.path, 'managed-buffers', count=5000)
        self.assertNotIn('fps', inspect_run(self.path, 'managed-buffers', 1, suite()))

    def test_prefixed_present_keeps_exact_count_and_window(self):
        write_run(self.path, 'managed-buffers')
        log = self.path / 'managed-buffers-1.log'
        log.write_text(log.read_text().replace('NATIVE_PRESENT frame=230 ',
            'RESULT NtCreateEvent type=0NATIVE_PRESENT frame=230 '))
        result = inspect_run(self.path, 'managed-buffers', 1, suite())
        self.assertEqual(result['errors'], [])
        self.assertEqual(result['racing_frames'], 5400)
        self.assertAlmostEqual(result['fps'], 50)

    def test_full_frame_count_does_not_hide_crash(self):
        write_run(self.path, 'managed-buffers', stop=False)
        self.assertNotIn('fps', inspect_run(self.path, 'managed-buffers', 1, suite()))

    def test_fault_before_expected_stop_is_failure(self):
        write_run(self.path, 'verify-buffers')
        with (self.path / 'verify-buffers-1.log').open('a') as log:
            log.write('NATIVE_GRAPHICS_DRED page_fault=0x2000\nSTOP present-limit test\n')
        self.assertTrue(inspect_run(self.path, 'verify-buffers', 1, suite())['errors'])

    def test_hash_and_rendering_settings_must_match(self):
        write_run(self.path, 'codex-before')
        meta_file = self.path / 'codex-before-1.json'
        meta = json.loads(meta_file.read_text())
        meta['sha256'] = 'C' * 64
        meta['settings']['SFR_RENDER_EVERY'] = '2'
        meta_file.write_text(json.dumps(meta))
        result = inspect_run(self.path, 'codex-before', 1, suite())
        self.assertEqual(len(result['errors']), 2)
        self.assertNotIn('fps', result)

    def test_verification_has_no_performance_numbers(self):
        write_run(self.path, 'verify-buffers')
        result = inspect_run(self.path, 'verify-buffers', 1, suite())
        self.assertEqual(result['errors'], [])
        self.assertNotIn('fps', result)

    def test_new_candidate_requires_independent_texture_flag(self):
        for config in ('codex-after', 'managed-buffers', 'verify-buffers'):
            for value in (None, '0'):
                with self.subTest(config=config, value=value):
                    write_run(self.path, config)
                    self.change_settings(config, SFR_MANAGED_TEXTURE_STATE=value)
                    result = inspect_run(self.path, config, 1, suite())
                    self.assertIn('Unexpected SFR_MANAGED_TEXTURE_STATE', result['errors'])
                    self.assertNotIn('fps', result)

    def test_before_and_control_reject_texture_flag(self):
        for config in ('codex-before', 'codex-control'):
            with self.subTest(config=config):
                write_run(self.path, config)
                self.change_settings(config, SFR_MANAGED_TEXTURE_STATE='1')
                result = inspect_run(self.path, config, 1, suite())
                self.assertIn('Unexpected SFR_MANAGED_TEXTURE_STATE', result['errors'])
                self.assertNotIn('fps', result)

    def test_new_verification_requires_texture_and_constant_checks(self):
        for key in ('SFR_VERIFY_MANAGED_TEXTURE_STATE', 'SFR_VERIFY_MANAGED_CONSTANTS'):
            for value in (None, '0'):
                with self.subTest(key=key, value=value):
                    write_run(self.path, 'verify-buffers')
                    self.change_settings('verify-buffers', **{key: value})
                    result = inspect_run(self.path, 'verify-buffers', 1, suite())
                    self.assertIn(f'Unexpected {key}', result['errors'])
                    self.assertNotIn('fps', result)

    def test_measured_runs_reject_texture_verification(self):
        for config in ('codex-before', 'codex-control', 'codex-after', 'managed-buffers'):
            with self.subTest(config=config):
                write_run(self.path, config)
                self.change_settings(config, SFR_VERIFY_MANAGED_TEXTURE_STATE='1')
                result = inspect_run(self.path, config, 1, suite())
                self.assertIn('Diagnostic setting present in measured run: SFR_VERIFY_MANAGED_TEXTURE_STATE',
                              result['errors'])
                self.assertNotIn('fps', result)

    def test_historical_suites_accept_old_flags_and_reject_new_texture_flags(self):
        for config in ('codex-after', 'managed-buffers', 'verify-buffers'):
            for explicit_marker in (False, True):
                with self.subTest(config=config, explicit_marker=explicit_marker):
                    legacy = suite(managed_texture_state=False)
                    if explicit_marker:
                        legacy['managed_texture_state'] = False
                    write_run(self.path, config, managed_texture_state=False)
                    self.assertEqual(inspect_run(self.path, config, 1, legacy)['errors'], [])
                    self.change_settings(config, SFR_MANAGED_TEXTURE_STATE='1')
                    result = inspect_run(self.path, config, 1, legacy)
                    self.assertIn('Unexpected SFR_MANAGED_TEXTURE_STATE', result['errors'])
                    self.assertNotIn('fps', result)
                    self.change_settings(config, SFR_MANAGED_TEXTURE_STATE='0', SFR_VERIFY_MANAGED_TEXTURE_STATE='1')
                    self.assertIn('Unexpected SFR_VERIFY_MANAGED_TEXTURE_STATE',
                                  inspect_run(self.path, config, 1, legacy)['errors'])

    def test_new_candidate_requires_independent_stencil_flag(self):
        for config in ('codex-after', 'managed-buffers', 'verify-buffers'):
            for value in (None, '0'):
                with self.subTest(config=config, value=value):
                    write_run(self.path, config)
                    self.change_settings(config, SFR_MANAGED_STENCIL_STATE=value)
                    result = inspect_run(self.path, config, 1, suite())
                    self.assertIn('Unexpected SFR_MANAGED_STENCIL_STATE', result['errors'])
                    self.assertNotIn('fps', result)

    def test_before_and_control_reject_stencil_flag(self):
        for config in ('codex-before', 'codex-control'):
            with self.subTest(config=config):
                write_run(self.path, config)
                self.change_settings(config, SFR_MANAGED_STENCIL_STATE='1')
                result = inspect_run(self.path, config, 1, suite())
                self.assertIn('Unexpected SFR_MANAGED_STENCIL_STATE', result['errors'])
                self.assertNotIn('fps', result)

    def test_new_verification_requires_stencil_and_constant_checks(self):
        for key in ('SFR_VERIFY_MANAGED_STENCIL_STATE', 'SFR_VERIFY_MANAGED_CONSTANTS'):
            for value in (None, '0'):
                with self.subTest(key=key, value=value):
                    write_run(self.path, 'verify-buffers')
                    self.change_settings('verify-buffers', **{key: value})
                    result = inspect_run(self.path, 'verify-buffers', 1, suite())
                    self.assertIn(f'Unexpected {key}', result['errors'])
                    self.assertNotIn('fps', result)

    def test_measured_runs_reject_stencil_ultrawide_and_race_diagnostics(self):
        for config in ('codex-before', 'codex-control', 'codex-after', 'managed-buffers'):
            for key in ('SFR_VERIFY_MANAGED_STENCIL_STATE', 'SFR_UW_DEBUG', 'SFR_UW_ISOLATE', 'SFR_RACE_PROGRESS',
                        'SFR_GAMEPLAY_METRICS', 'SFR_GAMEPLAY_TICK_LIMIT', 'SFR_DRAW_SIGNATURE_TRACE',
                        'SFR_TEXTURE_TIMERS', 'SFR_TEXTURE_TRACE'):
                with self.subTest(config=config, key=key):
                    write_run(self.path, config)
                    self.change_settings(config, **{key: '1'})
                    result = inspect_run(self.path, config, 1, suite())
                    self.assertIn(f'Diagnostic setting present in measured run: {key}', result['errors'])
                    self.assertNotIn('fps', result)

    def test_historical_suites_accept_old_flags_and_reject_new_stencil_flags(self):
        for config in ('codex-after', 'managed-buffers', 'verify-buffers'):
            for explicit_marker in (False, True):
                with self.subTest(config=config, explicit_marker=explicit_marker):
                    legacy = suite(managed_stencil_state=False)
                    if explicit_marker:
                        legacy['managed_stencil_state'] = False
                    write_run(self.path, config, managed_stencil_state=False)
                    self.assertEqual(inspect_run(self.path, config, 1, legacy)['errors'], [])
                    self.change_settings(config, SFR_MANAGED_STENCIL_STATE='1')
                    result = inspect_run(self.path, config, 1, legacy)
                    self.assertIn('Unexpected SFR_MANAGED_STENCIL_STATE', result['errors'])
                    self.assertNotIn('fps', result)
                    self.change_settings(config, SFR_MANAGED_STENCIL_STATE='0', SFR_VERIFY_MANAGED_STENCIL_STATE='1')
                    self.assertIn('Unexpected SFR_VERIFY_MANAGED_STENCIL_STATE',
                                  inspect_run(self.path, config, 1, legacy)['errors'])

    def test_all_managed_mismatches_fail_even_with_expected_stop(self):
        for kind in ('buffer', 'constants', 'short-constants', 'texture-state', 'stencil-state'):
            with self.subTest(kind=kind):
                write_run(self.path, 'verify-buffers')
                with (self.path / 'verify-buffers-1.log').open('a') as log:
                    log.write(f'STOP managed-{kind}-mismatch @0x1234: test\nSTOP present-limit test\n')
                result = inspect_run(self.path, 'verify-buffers', 1, suite())
                self.assertTrue(any(error.startswith('Correctness/GPU failure:') for error in result['errors']))
                self.assertNotIn('fps', result)

    def test_historical_complete_report_still_valid(self):
        root = self.path.parent
        (root / 'suite.json').write_text(json.dumps(suite(managed_texture_state=False, managed_stencil_state=False)))
        for stage, configs in [('verify', ['verify-buffers']), ('control', ['codex-before', 'codex-control']),
                               ('compare', ['codex-before', 'codex-after', 'managed-buffers'])]:
            for config in configs:
                write_run(root / stage, config, managed_texture_state=False, managed_stencil_state=False)
        self.assertTrue(build_report(root))
        self.assertIn('Candidate texture-state cache: disabled (historical suite).',
                      (root / 'report.md').read_text())

    def test_full_report_requires_all_runs(self):
        root = self.path.parent
        (root / 'suite.json').write_text(json.dumps(suite()))
        for stage, configs in [('verify', ['verify-buffers']), ('control', ['codex-before', 'codex-control']),
                               ('compare', ['codex-before', 'codex-after', 'managed-buffers'])]:
            for config in configs:
                write_run(root / stage, config)
        self.assertTrue(build_report(root))
        report = (root / 'report.md').read_text()
        self.assertIn('NOT a stable-60-Hz acceptance', report)
        self.assertIn('Candidate texture-state cache: enabled.', report)
        self.assertIn('Candidate stencil-state publication: enabled.', report)
        self.assertIn('equal record indices do NOT prove equal gameplay progress', report)
        measurements = json.loads((root / 'measurements.json').read_text())
        self.assertTrue(measurements['valid'])  # Integrity remains independent.
        self.assertFalse(measurements['gameplay_alignment_verified'])
        self.assertEqual(measurements['window_basis'], 'legacy_race_object_present')
        self.assertTrue(all(not run['gameplay_alignment_verified'] for run in measurements['runs']))
        self.assertTrue(all(run['frames'] == 3300 for run in measurements['runs'] if 'fps' in run))
        (root / 'compare/managed-buffers-1.json').unlink()
        self.assertFalse(build_report(root))


@unittest.skipUnless(os.name == 'nt' and shutil.which('powershell'), 'Windows PowerShell required')
class I5StagingTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.kit = self.base / 'kit with spaces'
        (self.kit / 'scripts').mkdir(parents=True)
        shutil.copy2(ROOT / 'scripts/benchmark_codex_i5.ps1', self.kit / 'scripts')
        (self.kit / 'play').mkdir()
        for name in ('image', 'assets'):
            (self.base / name).mkdir()
            (self.base / name / 'original').write_text(name)
        (self.kit / 'play/settings.ini').write_text(f'image_directory={self.base / "image"}\nasset_directory={self.base / "assets"}\n')
        (self.kit / 'sentinel').write_text('keep')
        manifest = suite()
        manifest['files'] = [dict(path='sentinel', sha256=hashlib.sha256(b'keep').hexdigest())]
        (self.kit / 'manifest.json').write_text(json.dumps(manifest))

    def invoke(self, prepare=True):
        env = dict(os.environ)
        env.pop('PSModulePath', None)
        return subprocess.run([shutil.which('powershell'), '-NoProfile', '-File',
                               str(self.kit / 'scripts/benchmark_codex_i5.ps1'), *(['-PrepareOnly'] if prepare else []),
                               '-Repeats', '1', '-ControlRepeats', '1',
                               '-LocalRoot', str(self.base / 'local'), '-ResultsRoot', str(self.base / 'export')],
                              env=env, capture_output=True, text=True, errors='replace', timeout=45)

    def test_paths_staged_locally_and_sources_preserved(self):
        result = self.invoke()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        exported = next((self.base / 'export').glob('*/suite.json'))
        state = json.loads(exported.read_text(encoding='utf-8-sig'))
        self.assertEqual(state['status'], 'prepared')
        self.assertIs(state['managed_texture_state'], True)
        self.assertIs(state['managed_stencil_state'], True)
        runtime = Path(state['local_runtime'])
        self.assertEqual(Path(state['game_paths']['image_directory']), self.base / 'image')
        self.assertEqual(Path(state['game_paths']['asset_directory']), self.base / 'assets')
        self.assertFalse(runtime.joinpath('game').exists())  # Already-local input is reused read-only.
        self.assertEqual((self.kit / 'sentinel').read_text(), 'keep')
        self.assertEqual((self.base / 'assets/original').read_text(), 'assets')
        self.assertEqual(list((self.base / 'export').rglob('*.zip')), [])
        # A second run must create a new folder, preserving the prior result.
        self.assertEqual(self.invoke().returncode, 0)
        self.assertEqual(len(list((self.base / 'export').glob('*/suite.json'))), 2)

    def test_corrupt_kit_stops_before_game_data_copy_and_exports_failure(self):
        (self.kit / 'sentinel').write_text('changed')
        result = self.invoke()
        self.assertNotEqual(result.returncode, 0)
        state = json.loads(next((self.base / 'export').glob('*/suite.json')).read_text(encoding='utf-8-sig'))
        self.assertEqual(state['status'], 'failed')
        self.assertIn('checksum mismatch', state['error'])
        self.assertFalse((Path(state['local_runtime']) / 'game').exists())

    def test_all_stages_use_strict_uncompressed_runner_and_generate_report(self):
        scripts = self.kit / 'scripts'
        for name in ('benchmark_codex_i5_report.py', 'benchmark_summary.py'):
            shutil.copy2(ROOT / 'scripts' / name, scripts / name)
        shutil.copy2(Path(__file__), scripts / 'test_benchmark_i5.py')
        (scripts / 'fake_run.py').write_text('''import sys
from pathlib import Path
from test_benchmark_i5 import write_run
for config in sys.argv[2].split(','):
    write_run(Path(sys.argv[1]), config)
''')
        (scripts / 'benchmark.ps1').write_text('''param([string[]]$Configs, [int]$Repeats, [string]$Out,
    [switch]$NoWarmup, [switch]$StopOnFailure, [switch]$NoShareArchive)
if (-not $StopOnFailure -or -not $NoShareArchive) { throw 'Missing strict/uncompressed flags' }
& python (Join-Path $PSScriptRoot 'fake_run.py') $Out ($Configs -join ',')
if ($LASTEXITCODE -ne 0) { throw 'Fake run generation failed' }
''')
        manifest = json.loads((self.kit / 'manifest.json').read_text())
        for path in scripts.iterdir():
            if path.is_file():
                manifest['files'].append(dict(path=str(path.relative_to(self.kit)), sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
        (self.kit / 'manifest.json').write_text(json.dumps(manifest))
        result = self.invoke(prepare=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        exported = next((self.base / 'export').glob('*/suite.json'))
        state = json.loads(exported.read_text(encoding='utf-8-sig'))
        self.assertEqual(state['status'], 'completed')
        self.assertIs(state['managed_texture_state'], True)
        self.assertIs(state['managed_stencil_state'], True)
        self.assertEqual([s['name'] for s in state['stages']], ['verify', 'control', 'compare'])
        self.assertTrue(all(s['status'] == 'passed' for s in state['stages']))
        self.assertTrue(json.loads((exported.parent / 'measurements.json').read_text())['valid'])

    def test_failed_stage_keeps_original_cause_when_report_also_fails(self):
        scripts = self.kit / 'scripts'
        for name in ('benchmark_codex_i5_report.py', 'benchmark_summary.py'):
            shutil.copy2(ROOT / 'scripts' / name, scripts / name)
        (scripts / 'benchmark.ps1').write_text("throw 'synthetic game failure: keep this original cause'\n")
        manifest = json.loads((self.kit / 'manifest.json').read_text())
        for path in scripts.iterdir():
            if path.is_file():
                manifest['files'].append(dict(path=str(path.relative_to(self.kit)), sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
        (self.kit / 'manifest.json').write_text(json.dumps(manifest))
        result = self.invoke(prepare=False)
        self.assertNotEqual(result.returncode, 0)
        exported = next((self.base / 'export').glob('*/suite.json'))
        state = json.loads(exported.read_text(encoding='utf-8-sig'))
        self.assertEqual(state['status'], 'failed')
        self.assertEqual(state['stages'][0]['status'], 'failed')
        self.assertIn('synthetic game failure', state['error'])
        self.assertIn('Result validation failed', state['validation_error'])


if __name__ == '__main__':
    unittest.main()
