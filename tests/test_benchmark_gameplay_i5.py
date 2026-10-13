"""Synthetic semantic-suite fixtures; these never launch the game."""
import json
from pathlib import Path
import sys
import shutil
import unittest
import uuid
import subprocess

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from benchmark_codex_gameplay_report import (
    EXPECTED_SETTINGS, GAME_CONFIG, ROUTE, VERIFY_SETTINGS, build_report,
    fixture_hashes, inspect_resume, inspect_run, inspect_suite, observed_texture_timing,
    quiet_context, stage_plan, validate_suite)


def manifest():
    suite = dict(protocol='gameplay_v1', status='completed', baseline_sha256='a' * 64,
                 candidate_sha256='b' * 64, repeats=6, control_repeats=2,
                 gameplay_ticks=5400, skip=1200, window_end=4500,
                 expected_game_config=GAME_CONFIG[:], route_say=ROUTE,
                 fixture_files=[dict(path='E000/profile.bin', sha256='c' * 64)])
    suite['stages'] = [dict(name=name, configs=configs, repeats=count, status='passed')
                       for name, configs, count in stage_plan(suite)]
    return suite


def log_fixture(duration=10):
    lines = []
    for frame in range(5402):
        tick = max(0, frame - 1)
        cd, race = min(tick, 300), max(0, tick - 300)
        row = dict(frame=frame, draw_timers=0, presented=1, frame_ms=duration, texture_timers=0,
                   racing=1, gameplay_schema=1, game_contract='solo_fixed_v1', game_run=1,
                   game_state='prestart' if tick == 0 else 'advancing',
                   game_stage='none' if tick == 0 else 'countdown' if tick <= 300 else 'racing',
                   game_start_observed=int(tick > 0),
                   game_phase=9 if tick == 0 else 10 if tick < 300 else 11,
                   game_tick=tick, game_countdown_tick=cd, game_race_tick=race,
                   game_countdown_ms=f'{max(0, 5000 - cd * 16.666667938):.9f}',
                   game_elapsed_ms=f'{race * 16.666667938:.9f}',
                   game_config='1:2:1:0:3:60:0')
        lines.append('NATIVE_PRESENT ' + ' '.join(f'{k}={v}' for k, v in row.items()))
    return lines + ['STOP gameplay-limit @0x132: SFR_GAMEPLAY_TICK_LIMIT reached']


class SemanticSuiteTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lines = log_fixture()
        cls.log_text = '\n'.join(cls.lines) + '\n'

    def setUp(self):
        runtime = Path(__file__).resolve().parents[1] / 'out' / 'test-gameplay-report'
        runtime.mkdir(parents=True, exist_ok=True)
        # Normal mkdir inherits workspace ACLs; tempfile's private Windows ACL
        # excludes the restricted token used by sandboxed test execution.
        self.root = runtime / uuid.uuid4().hex
        self.root.mkdir()
        self.addCleanup(shutil.rmtree, self.root)
        self.suite = manifest()
        self.save_suite()

    def save_suite(self):
        (self.root / 'suite.json').write_text(json.dumps(self.suite), encoding='utf-8-sig')

    def write_run(self, stage='compare', config='semantic-after', repeat=1, lines=None, settings=None):
        directory = self.root / stage
        directory.mkdir(exist_ok=True)
        (directory / 'fixture-hashes.json').write_text(json.dumps(
            dict(path='\\E000\\profile.bin', sha256='C' * 64)), encoding='utf-8-sig')
        old = config in ('semantic-before', 'semantic-control')
        effective = dict(EXPECTED_SETTINGS, SFR_TEXTURE_TIMERS='0')
        if old:
            effective.pop('SFR_TEXTURE_TIMERS')
        if config == 'semantic-verify':
            effective.update({key: '1' for key in VERIFY_SETTINGS})
            effective.update(SFR_SCREENSHOT='verify-%d.bmp', SFR_SCREENSHOT_EVERY='1500')
        effective.update(settings or {})
        metadata = dict(sha256=self.suite['baseline_sha256' if old else 'candidate_sha256'],
                        settings=effective, exit_code=3)
        stem = directory / f'{config}-{repeat}'
        stem.with_suffix('.json').write_text(json.dumps(metadata), encoding='utf-8-sig')
        text = self.log_text if lines is None else '\n'.join(lines)
        if old:
            text = text.replace(' texture_timers=0', '')
        stem.with_suffix('.log').write_text(text, encoding='utf-8')
        return stem

    def write_suite(self):
        for stage, configs, count in stage_plan(self.suite):
            for repeat in range(1, count + 1):
                for config in configs:
                    self.write_run(stage, config, repeat)

    def inspect(self, stage='compare', config='semantic-after', repeat=1):
        return inspect_run(self.root / stage, config, repeat, self.suite)

    def test_complete_suite_reports_all_segments_pairs_and_limits(self):
        self.write_suite()
        result = inspect_suite(self.root)
        self.assertTrue(result['valid'], result['errors'])
        self.assertEqual(len(result['runs']), 17)
        for run in result['runs']:
            self.assertEqual(run['simulation_ticks'], 5400)
            self.assertEqual([run[k]['frames'] for k in ('countdown', 'racing', 'combined', 'race_window')],
                             [300, 5100, 5400, 3300])
        self.assertEqual(result['paired_ratios']['compare']['combined']['pairs'], 6)
        self.assertEqual(result['paired_ratios']['control']['countdown']['median_ratio'], 1)
        self.assertTrue(all(v['statistical_target_met'] for v in result['statistical_thresholds'].values()))
        self.assertTrue(build_report(self.root))
        text = (self.root / 'gameplay-report.md').read_text()
        self.assertIn('NOT VERIFIED', text)
        self.assertIn('worst replicate', text)
        self.assertFalse(result['displayed_60hz_verified'])
        self.assertFalse(result['input_latency_verified'])
        self.assertFalse(result['heavy_free_race_verified'])

    def test_manifest_schema_route_bounds_status_and_stages_are_required(self):
        mutations = [('protocol', 'legacy'), ('gameplay_ticks', 5100), ('repeats', 5),
                     ('repeats', True), ('control_repeats', 1), ('control_repeats', 100000),
                     ('skip', 0), ('window_end', 4400), ('baseline_sha256', 'unknown'),
                     ('candidate_sha256', None), ('expected_game_config', [1, 2]),
                     ('route_say', ROUTE + ',ok@5000'), ('status', 'running'), ('stages', [])]
        for key, value in mutations:
            with self.subTest(key=key, value=value):
                suite = manifest()
                suite[key] = value
                self.assertTrue(validate_suite(suite))
        suite = manifest()
        suite['stages'][2]['configs'].append('managed-buffers')
        self.assertTrue(validate_suite(suite))
        suite = manifest()
        suite['stages'][0]['status'] = 'running'
        self.assertTrue(validate_suite(suite))
        self.assertTrue(validate_suite([]))

    def test_missing_runs_metadata_and_hash_mismatch_fail(self):
        self.assertFalse(inspect_suite(self.root)['valid'])
        stem = self.write_run()
        stem.with_suffix('.json').unlink()
        self.assertFalse(self.inspect()['valid'])
        stem = self.write_run()
        meta = json.loads(stem.with_suffix('.json').read_text(encoding='utf-8-sig'))
        meta['sha256'] = self.suite['baseline_sha256']
        stem.with_suffix('.json').write_text(json.dumps(meta))
        self.assertIn('hash', ' '.join(self.inspect()['errors']))
        stem.with_suffix('.json').write_text('[]')
        self.assertFalse(self.inspect()['valid'])

    def test_matching_stop_still_requires_expected_process_exit_and_preserves_admission(self):
        stem = self.write_run()
        path = stem.with_suffix('.json')
        original = json.loads(path.read_text(encoding='utf-8-sig'))
        for exit_code in (None, 0, -1073741819, '3', True):
            with self.subTest(exit_code=exit_code):
                meta = dict(original)
                if exit_code is None:
                    del meta['exit_code']
                else:
                    meta['exit_code'] = exit_code
                path.write_text(json.dumps(meta))
                self.assertFalse(self.inspect()['valid'])
                self.assertIn('exit_code', ' '.join(self.inspect()['errors']))
        original['admission'] = dict(cpu_percent=8, game_processes=0, admitted=True)
        path.write_text(json.dumps(original))
        result = self.inspect()
        self.assertTrue(result['valid'])
        self.assertEqual(result['admission'], original['admission'])

    def test_settings_route_rendering_and_formal_diagnostics_fail(self):
        # Keep the progress parser mocked here: this test is solely the metadata
        # admission policy. Real parser integration is covered below and above.
        from unittest.mock import patch
        from benchmark_gameplay import inspect_gameplay
        stem = self.write_run()
        progress = inspect_gameplay(stem.with_suffix('.log'))
        mutations = dict(SFR_MANAGED_CONSTANTS='0', SFR_MANAGED_TEXTURE_STATE='0',
            SFR_MANAGED_STENCIL_STATE='0', SFR_MANAGED_BUFFERS='1', SFR_DIRECT_CONSTANT_UPLOAD='1',
            SFR_REALTIME_RACE='1', SFR_RENDER_EVERY='2', SFR_ALLOW_RENDER_TARGETS='0',
            SFR_REALTIME_UI='0', SFR_SAY_REFERENCE_FPS='100', SFR_SAY_MIN_SECONDS='0',
            SFR_GPU_PIPELINE='0', SFR_VERTEX_CACHE='0', SFR_SKIP_FOREIGN_TARGETS='1',
            SFR_AUDIO='0', SFR_DRAW_TIMERS='1', SFR_FRAME_METRICS='0', SFR_RENDER_SCALE='50',
            SFR_GRAPHICS='vulkan', SFR_ASPECT_MODE='auto', SFR_UI_ALIGN='center', SFR_VSYNC='1',
            SFR_FULLSCREEN='1', SFR_PRESENT_LIMIT='5400', SFR_PRESENT_LIMIT_RACING='5400',
            SFR_PRESENT_LIMIT_AFTER_SAY='5400', SFR_FRAME_LIMIT='60', SFR_PROFILE='0',
            SFR_GAMEPLAY_METRICS='0', SFR_GAMEPLAY_TICK_LIMIT='5100', SFR_SAY='wrong route',
            SFR_PLAYER1_INPUT='both', SFR_PLAYER2_INPUT='keyboard', SFR_NUI_HAND_CENTRED='0',
            SFR_SCREENSHOT='file.bmp', SFR_SCREENSHOT_EVERY='1', SFR_TRACE_INPUT='1',
            SFR_RACE_PROGRESS='1', SFR_UW_DEBUG='1', SFR_BUFFER_AUDIT='1',
            SFR_TEXTURE_TIMERS='1', SFR_TEXTURE_TRACE='1', SFR_DRAW_SIGNATURE_TRACE='1',
            SFR_STALL_MS='50', SFR_MAIN_PROFILE='1', SFR_PROFILE_GUEST='1',
            SFR_HOST_PROFILE='1', SFR_SAMPLE_PROFILE='1',
            SFR_VERIFY_MANAGED_CONSTANTS='1', SFR_D3D12_DRED='1')
        with patch('benchmark_codex_gameplay_report.inspect_gameplay', return_value=progress):
            self.assertTrue(self.inspect()['valid'])
            for key, value in mutations.items():
                with self.subTest(key=key):
                    self.write_run(settings={key: value})
                    self.assertFalse(self.inspect()['valid'])
            stem = self.write_run()
            meta = json.loads(stem.with_suffix('.json').read_text(encoding='utf-8-sig'))
            del meta['settings']['SFR_MANAGED_STENCIL_STATE']
            stem.with_suffix('.json').write_text(json.dumps(meta))
            self.assertFalse(self.inspect()['valid'])

    def test_both_versions_use_managed_features_and_old_texture_marker_is_optional(self):
        for stage, config in [('compare', 'semantic-before'), ('control', 'semantic-control'),
                              ('verify', 'semantic-verify'), ('compare', 'semantic-after')]:
            self.write_run(stage, config)
            self.assertTrue(self.inspect(stage, config)['valid'])
        stem = self.write_run()
        meta = json.loads(stem.with_suffix('.json').read_text(encoding='utf-8-sig'))
        del meta['settings']['SFR_TEXTURE_TIMERS']
        stem.with_suffix('.json').write_text(json.dumps(meta))
        self.assertFalse(self.inspect()['valid'])
        self.write_run('compare', 'semantic-before', settings={'SFR_MANAGED_TEXTURE_STATE': '0'})
        self.assertFalse(self.inspect('compare', 'semantic-before')['valid'])

    def test_legacy_schema_gaps_stops_configs_and_faults_fail(self):
        for change in ('schema', 'gap', 'tick', 'config', 'unexpected_config', 'stop', 'fault', 'omitted'):
            with self.subTest(change=change):
                lines = self.lines[:]
                if change == 'schema':
                    lines[4] = lines[4].replace('gameplay_schema=1', 'gameplay_schema=0')
                elif change == 'gap':
                    del lines[4]
                elif change == 'tick':
                    lines[4] = lines[4].replace('game_tick=3 ', 'game_tick=2 ')
                elif change == 'config':
                    lines[4] = lines[4].replace('game_config=1:', 'game_config=2:')
                elif change == 'unexpected_config':
                    lines = [line.replace('game_config=1:', 'game_config=2:') for line in lines]
                elif change == 'stop':
                    lines[-1] = 'STOP present-limit @0 old counter'
                elif change == 'fault':
                    lines.insert(-1, 'NATIVE_GRAPHICS_DRED page_fault=bad')
                else:
                    lines[4] = lines[4].replace('presented=1', 'presented=0')
                self.write_run(lines=lines)
                self.assertFalse(self.inspect()['valid'])

    def test_final_countdown_stall_is_retained_and_worst_round_cannot_hide_tail(self):
        self.write_suite()
        lines = self.lines[:]
        # Tick300 is phase11/elapsed0 and still part of countdown cost.
        lines[301] = lines[301].replace('frame_ms=10 ', 'frame_ms=800 ')
        self.write_run(lines=lines)
        run = self.inspect()
        self.assertTrue(run['valid'])
        self.assertEqual(run['countdown']['max_ms'], 800)
        self.assertEqual(run['racing']['max_ms'], 10)
        # A single bad replicate fails P95 even though five other replicates
        # leave the paired median unchanged.
        for frame in range(2, 32):
            lines[frame] = lines[frame].replace('frame_ms=10 ', 'frame_ms=30 ')
        self.write_run(lines=lines)
        result = inspect_suite(self.root)
        target = result['statistical_thresholds']['compare/semantic-after/countdown']
        self.assertTrue(result['valid'])
        self.assertFalse(target['statistical_target_met'])
        self.assertEqual(target['worst_replicate_p95_ms'], 30)
        self.assertEqual(result['paired_ratios']['compare']['countdown']['median_ratio'], 1)
        lines[301] = lines[301].replace('game_stage=countdown', 'game_stage=racing')
        self.write_run(lines=lines)
        self.assertFalse(self.inspect()['valid'])

    def test_legacy_input_files_and_reports_are_never_modified_or_consumed(self):
        self.suite.pop('protocol')
        self.save_suite()
        protected = {}
        for name in ('measurements.json', 'report.md', 'compare/codex-before-1.log'):
            path = self.root / name
            path.parent.mkdir(exist_ok=True)
            path.write_text('historical bytes')
            protected[path] = path.read_bytes()
        before_suite = (self.root / 'suite.json').read_bytes()
        self.assertFalse(build_report(self.root))
        self.assertEqual({path: path.read_bytes() for path in protected}, protected)
        self.assertEqual((self.root / 'suite.json').read_bytes(), before_suite)
        result = json.loads((self.root / 'gameplay-measurements.json').read_text())
        self.assertEqual(result['runs'], [])
        self.assertIn('legacy protocol is not consumed', ' '.join(result['errors']))

    def test_quiet_context_is_explicitly_unknown_or_pre_stage_only(self):
        self.assertEqual(quiet_context(self.root)['control']['status'], 'unknown')
        (self.root / 'control').mkdir()
        path = self.root / 'control' / 'info.txt'
        path.write_text('power=no battery (desktop) plan=High performance idle_cpu_percent=26\n')
        context = quiet_context(self.root)['control']
        self.assertEqual(context['status'], 'review_required')
        self.assertEqual(context['admission_cpu_percent'], 26)
        self.assertFalse(context['quiet_throughout_runs_verified'])
        path.write_text('power=no battery (desktop) plan=High performance idle_cpu_percent=0\n')
        self.assertEqual(quiet_context(self.root)['control']['status'], 'recorded_pre_stage_only')

    def test_fixture_hash_sets_paths_and_duplicates_are_strict(self):
        expected = fixture_hashes(self.suite['fixture_files'])
        self.assertEqual(fixture_hashes(dict(path='\\E000\\profile.bin', sha256='C' * 64)), expected)
        for records in ([], None, [self.suite['fixture_files'][0]] * 2,
                        [dict(path='../save', sha256='c' * 64)],
                        [dict(path='save', sha256='unknown')]):
            with self.subTest(records=records), self.assertRaises(ValueError):
                fixture_hashes(records)
        self.write_suite()
        path = self.root / 'control' / 'fixture-hashes.json'
        path.write_text(json.dumps(dict(path='different/save', sha256='c' * 64)))
        result = inspect_suite(self.root)
        self.assertFalse(result['valid'])
        self.assertIn('fixture', ' '.join(result['errors']))
        path.unlink()
        self.assertFalse(inspect_suite(self.root)['valid'])

    def test_texture_timing_comes_from_present_marker_and_sources_from_suite(self):
        stem = self.write_run()
        self.assertEqual(self.inspect()['texture_timing'], 'disabled')
        self.write_run(lines=[line.replace('texture_timers=0', 'texture_timers=1') for line in self.lines])
        self.assertFalse(self.inspect()['valid'])
        self.write_run('compare', 'semantic-before')
        self.assertEqual(self.inspect('compare', 'semantic-before')['texture_timing'],
                         'enabled (legacy marker absent)')
        for marker, expected in [('0', 'disabled'), ('1', 'enabled')]:
            stem.with_suffix('.log').write_text('NATIVE_PRESENT frame=0 texture_timers=' + marker)
            self.assertEqual(observed_texture_timing(stem.with_suffix('.log')), expected)
        self.suite.update(baseline_commit='d' * 40, candidate_runtime_commit='e' * 40,
                          candidate_commit='f' * 40)
        self.save_suite()
        self.write_suite()
        self.assertTrue(build_report(self.root))
        text = (self.root / 'gameplay-report.md').read_text()
        for value in ('d' * 40, 'e' * 40, 'f' * 40):
            self.assertIn(value, text)
        self.assertNotIn('4e72384', text)
        self.assertIn('does not estimate total R1-R4 benefit', text)

    def write_resume_boundary(self):
        self.suite['status'] = 'failed'
        self.suite['stages'] = self.suite['stages'][:2]
        self.suite.update(started='2026-10-13T19:25:26+08:00', completed='2026-10-13T19:47:00+08:00',
                          error='CPU 41% at compare admission',
                          preflight=[dict(stage='control', cpu_percent=8, game_processes=0, admitted=True)])
        self.save_suite()
        for stage, configs, count in stage_plan(self.suite)[:2]:
            for repeat in range(1, count + 1):
                for config in configs:
                    self.write_run(stage, config, repeat)

    def test_resume_exact_boundary_is_read_only_and_normal_report_still_invalid(self):
        self.write_resume_boundary()
        before = {p.relative_to(self.root): p.read_bytes() for p in self.root.rglob('*') if p.is_file()}
        result = inspect_resume(self.root)
        self.assertTrue(result['resume_valid'], result['errors'])
        self.assertEqual(result['source_params']['repeats'], 6)
        self.assertEqual(result['source_params']['fixture_files'], self.suite['fixture_files'])
        self.assertEqual(result['source_timestamps']['completed'], self.suite['completed'])
        self.assertEqual(result['stages'], self.suite['stages'])
        self.assertEqual(result['preflight'], self.suite['preflight'])
        self.assertEqual(result['source_suite'], self.suite)
        for key in ('runs', 'paired_ratios', 'statistical_thresholds', 'fps'):
            self.assertNotIn(key, result)
        self.assertFalse(inspect_suite(self.root)['valid'])
        self.assertEqual({p.relative_to(self.root): p.read_bytes() for p in self.root.rglob('*') if p.is_file()}, before)
        script = Path(__file__).resolve().parents[1] / 'scripts' / 'benchmark_codex_gameplay_report.py'
        cli = subprocess.run([sys.executable, str(script), '--resume-check', str(self.root)],
                             text=True, capture_output=True, check=False)
        self.assertEqual(cli.returncode, 0, cli.stderr)
        self.assertTrue(json.loads(cli.stdout)['resume_valid'])
        self.assertEqual({p.relative_to(self.root): p.read_bytes() for p in self.root.rglob('*') if p.is_file()}, before)

    def test_resume_refuses_changed_boundary_counts_route_and_partial_compare(self):
        self.write_resume_boundary()
        original = json.loads(json.dumps(self.suite))
        for key, value in [('status', 'completed'), ('protocol', 'legacy'), ('repeats', 7),
                           ('control_repeats', 3), ('route_say', 'changed'),
                           ('gameplay_ticks', 5100), ('expected_game_config', [1, 2])]:
            with self.subTest(key=key):
                self.suite = dict(original, **{key: value})
                self.save_suite()
                self.assertFalse(inspect_resume(self.root)['resume_valid'])
        self.suite = json.loads(json.dumps(original))
        self.suite['stages'].append(dict(name='compare', status='running'))
        self.save_suite()
        self.assertFalse(inspect_resume(self.root)['resume_valid'])
        self.suite = original
        self.save_suite()
        (self.root / 'compare').mkdir()
        self.assertFalse(inspect_resume(self.root)['resume_valid'])

    def test_resume_refuses_missing_runs_metadata_hash_fixture_and_progress(self):
        self.write_resume_boundary()
        stem = self.root / 'control' / 'semantic-control-2'
        metadata_path = stem.with_suffix('.json')
        original = metadata_path.read_bytes()
        metadata_path.unlink()
        self.assertFalse(inspect_resume(self.root)['resume_valid'])
        metadata_path.write_bytes(original)
        meta = json.loads(original.decode('utf-8-sig'))
        meta['sha256'] = 'd' * 64
        metadata_path.write_text(json.dumps(meta))
        self.assertFalse(inspect_resume(self.root)['resume_valid'])
        metadata_path.write_bytes(original)
        log_path = stem.with_suffix('.log')
        original_log = log_path.read_bytes()
        log_path.write_bytes(original_log.replace(b'game_tick=3 ', b'game_tick=2 ', 1))
        self.assertFalse(inspect_resume(self.root)['resume_valid'])
        log_path.unlink()
        self.assertFalse(inspect_resume(self.root)['resume_valid'])
        log_path.write_bytes(original_log)
        (self.root / 'verify' / 'fixture-hashes.json').write_text(json.dumps(
            dict(path='E000/profile.bin', sha256='d' * 64)))
        self.assertFalse(inspect_resume(self.root)['resume_valid'])

    def test_resumed_report_warns_about_reused_controls_and_requires_completion(self):
        self.write_resume_boundary()
        self.suite['resumed_from'] = 'original-local-session'
        self.suite['source_suite'] = dict(status='failed', completed=self.suite['completed'])
        self.suite['import_inventory'] = ['verify', 'control']
        self.save_suite()
        self.assertFalse(build_report(self.root))
        text = (self.root / 'gameplay-report.md').read_text()
        self.assertIn('Original verification and controls were reused across an interruption', text)
        result = json.loads((self.root / 'gameplay-measurements.json').read_text())
        self.assertEqual(result['resumed_from'], self.suite['resumed_from'])
        self.assertEqual(result['source_suite'], self.suite['source_suite'])
        self.suite['status'] = 'completed'
        self.suite['stages'].append(dict(name='compare', configs=['semantic-before', 'semantic-after'],
                                         repeats=6, status='passed'))
        self.save_suite()
        self.assertFalse(inspect_suite(self.root)['valid'])
        for repeat in range(1, 7):
            for config in ('semantic-before', 'semantic-after'):
                self.write_run('compare', config, repeat)
        self.assertTrue(inspect_suite(self.root)['valid'])


if __name__ == '__main__':
    unittest.main()
