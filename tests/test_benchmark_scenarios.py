import importlib.util
from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts' / 'benchmark_scenarios.py'


class BenchmarkScenariosTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(SCRIPT.is_file(), 'the portable scenario runner is missing')
        spec = importlib.util.spec_from_file_location('benchmark_scenarios', SCRIPT)
        self.runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.runner)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.package = self.root / 'package'
        self.package.mkdir()
        self.launcher = 'FreeRidersRecompiled.exe' if os.name == 'nt' else 'FreeRidersRecompiled'
        self.game = 'sfr_cpu_diagnostic.exe' if os.name == 'nt' else 'sfr_cpu_diagnostic'
        for name in (self.launcher, self.game, 'shaders.pack'):
            (self.package / name).write_bytes(name.encode())
        self.settings = self.root / 'settings.ini'
        self.settings.write_text('audio=0\nwindow_width=1920\nplayer2_device=keyboard\n')
        self.protocol = self.root / 'protocol.txt'
        self.protocol.write_text('Course: example. Character: Sonic. Capture: fixed HUD interval.')
        for directory, files in {'image': ['complete.txt', 'image.bin'],
                                 'assets': ['default.xex'], 'save': ['profile.bin']}.items():
            (self.root / directory).mkdir()
            for name in files:
                (self.root / directory / name).write_bytes(b'seed')

    def plan(self, *extra):
        args = self.runner.parse_args([
            '--package', str(self.package), '--settings', str(self.settings),
            '--image', str(self.root / 'image'), '--assets', str(self.root / 'assets'),
            '--save', str(self.root / 'save'), '--output', str(self.root / 'runs'),
            '--protocol', str(self.protocol),
            '--scenario', '1p', '--backend', 'vulkan', *extra])
        return self.runner.make_plan(args, {'PATH': 'host-path', 'SFR_FRAME_LIMIT': '999',
                                          'SFR_MAIN_PROFILE': '1', 'SFR_UNKNOWN': 'unsafe'})

    def test_dry_plan_needs_no_assets_and_does_not_write(self):
        plan = self.plan('--package', str(self.root / 'missing'), '--scenario', 'camera')
        self.assertEqual(plan['frame_limit'], 60)
        self.assertEqual(plan['validation'], 'not-validated')
        self.assertFalse((self.root / 'runs').exists())

    def test_resolution_is_separate_from_window_size_and_render_skip(self):
        plan = self.plan('--render-scale', '75')
        settings = self.runner.scenario_settings(plan, self.settings.read_text())
        self.assertIn('window_width=1920\n', settings)
        self.assertIn('render_scale=75\n', settings)
        self.assertIn('race_render_every=1\n', settings)

    def test_scenarios_separate_camera_avatar_and_second_player(self):
        for scenario in ('1p', '2p', 'camera', 'vrm'):
            plan = self.plan('--scenario', scenario, '--avatar-model', str(self.root / 'rider.vrm'))
            settings = self.runner.scenario_settings(plan, self.settings.read_text())
            self.assertIn('audio=0', settings)
            self.assertIn('voice=0', settings)
            self.assertIn('camera=' + ('motion' if scenario == 'camera' else 'off'), settings)
            self.assertIn('player2_device=' + ('keyboard' if scenario == '2p' else 'off'), settings)
            self.assertIn('avatar_model=' + (str((self.root / 'rider.vrm').resolve()) if scenario == 'vrm' else '') + '\n', settings)

    def test_validation_requires_assets_and_feature_inputs(self):
        self.runner.validate_plan(self.plan())
        with self.assertRaisesRegex(ValueError, 'pose'):
            self.runner.validate_plan(self.plan('--scenario', 'camera', '--camera-device', 'Webcam'))
        with self.assertRaisesRegex(ValueError, 'avatar'):
            self.runner.validate_plan(self.plan('--scenario', 'vrm'))
        (self.root / 'image' / 'complete.txt').unlink()
        with self.assertRaisesRegex(ValueError, 'complete.txt'):
            self.runner.validate_plan(self.plan())

    def test_optional_explicit_input_is_checked_before_staging(self):
        with self.assertRaisesRegex(ValueError, 'avatar'):
            self.runner.prepare_run(self.plan('--avatar-model', str(self.root / 'missing.vrm')))
        self.assertFalse((self.root / 'runs').exists())

    def test_output_cannot_overlap_package_or_seed(self):
        for path in (self.package / 'runs', self.root / 'save' / 'runs', self.root):
            with self.assertRaisesRegex(ValueError, 'overlap'):
                self.runner.validate_plan(self.plan('--output', str(path)))

    def test_stage_isolates_save_cache_settings_and_records_hashes(self):
        (self.package / 'game.log').write_text('stale')
        (self.package / 'settings.ini').write_text('user settings')
        (self.package / 'save').mkdir()
        (self.package / 'save' / 'original').write_text('untouched')
        cache = self.root / 'warm.bin'
        cache.write_bytes(b'warm-cache')
        plan = self.plan('--pipeline-cache', str(cache))
        run, metadata = self.runner.prepare_run(plan)
        staged = run / 'package'
        self.assertEqual((staged / 'save' / 'profile.bin').read_bytes(), b'seed')
        self.assertFalse((staged / 'save' / 'original').exists())
        self.assertFalse((staged / 'game.log').exists())
        self.assertEqual((staged / 'pipeline.bin').read_bytes(), b'warm-cache')
        (staged / 'save' / 'profile.bin').write_bytes(b'changed')
        self.assertEqual((self.root / 'save' / 'profile.bin').read_bytes(), b'seed')
        self.assertEqual((self.package / 'settings.ini').read_text(), 'user settings')
        self.assertEqual(metadata['status'], 'prepared')
        self.assertEqual(len(metadata['game']['sha256']), 64)
        self.assertEqual(metadata['internal_resolution']['verified'], False)
        env = self.runner.launch_environment(plan, staged, {'PATH': 'host', 'SFR_MAIN_PROFILE': '1', 'SFR_FRAME_LIMIT': '999'})
        self.assertEqual(env['PATH'], 'host')
        self.assertEqual(env['SFR_FRAME_LIMIT'], '60')
        self.assertNotIn('SFR_MAIN_PROFILE', env)
        self.assertEqual(env['SFR_FRAME_METRICS'], '1')
        self.assertEqual(env['SFR_TRACE_INPUT'], '0')
        self.assertTrue(Path(env['SFR_PIPELINE_CACHE_PATH']).is_relative_to(run))
        self.assertEqual(json.loads((run / 'run.json').read_text())['status'], 'prepared')

    def test_lock_rejects_overlap_and_releases_after_failure(self):
        lock = self.root / 'capture.lock'
        with self.runner.exclusive_run(lock):
            with self.assertRaisesRegex(RuntimeError, 'another'):
                with self.runner.exclusive_run(lock):
                    self.fail('overlapping capture was admitted')
        with self.runner.exclusive_run(lock):
            pass

    def fake_capture(self, log, exit_code=0, change_settings=False, rewrite_settings=None):
        plan = self.plan()

        class FakeLauncher:
            def __init__(child, command, cwd, env):
                self.assertEqual(env['SFR_FRAME_LIMIT'], '60')
                self.assertEqual(command, [str(cwd / self.launcher)])
                if log is not None:
                    (cwd / 'game.log').write_text(log)
                if change_settings:
                    with (cwd / 'settings.ini').open('a') as settings:
                        settings.write('race_render_every=2\n')
                if rewrite_settings:
                    settings = cwd / 'settings.ini'
                    rewritten = []
                    for line in settings.read_text().splitlines():
                        if '=' in line:
                            key, value = line.split('=', 1)
                            line = key + '=' + rewrite_settings.get(key, value)
                        rewritten.append(line)
                    settings.write_text('\n'.join(rewritten) + '\n')

            def wait(child):
                return exit_code

        # Linux may run uname through subprocess while resolving processor().
        # Keep host discovery outside this fixture's fake launcher boundary.
        with (patch.object(self.runner.platform, 'platform', return_value='fixture OS'),
              patch.object(self.runner.platform, 'processor', return_value='fixture CPU'),
              patch.object(self.runner.subprocess, 'Popen', FakeLauncher),
              redirect_stdout(io.StringIO())):
            result = self.runner.capture(plan)
        records = list((self.root / 'runs').glob('*/run.json'))
        self.assertEqual(len(records), 1)
        metadata = json.loads(records[0].read_text())
        self.assertEqual(metadata['host']['system'], 'fixture OS')
        self.assertEqual(metadata['host']['processor'], 'fixture CPU')
        return result, metadata

    def test_capture_cannot_claim_runtime_success_from_frames_after_crash(self):
        result, metadata = self.fake_capture('NATIVE_PRESENT frame=1\nNATIVE_PRESENT frame=2\n', exit_code=7)
        self.assertEqual(result, 1)
        self.assertEqual(metadata['status'], 'invalid-launcher-exit')

    def test_capture_rejects_runtime_stop_even_when_launcher_exits_successfully(self):
        result, metadata = self.fake_capture('NATIVE_PRESENT frame=1\nSTOP unimplemented\n')
        self.assertEqual(result, 1)
        self.assertEqual(metadata['status'], 'invalid-runtime-stop')

    def test_capture_rejects_diagnostic_exception_with_zero_launcher_exit(self):
        result, metadata = self.fake_capture('NATIVE_PRESENT frame=1\nDiagnostic error: allocation failed\n')
        self.assertEqual(result, 1)
        self.assertEqual(metadata['status'], 'invalid-runtime-stop')
        self.assertIn('Diagnostic error: allocation failed', metadata['runtime_evidence'])

    def test_capture_rejects_truncated_log_without_normal_child_termination(self):
        result, metadata = self.fake_capture('NATIVE_PRESENT frame=1\nNATIVE_PRESENT frame=2\n')
        self.assertEqual(result, 1)
        self.assertEqual(metadata['status'], 'invalid-incomplete-log')

    def test_capture_without_game_is_not_a_measurement(self):
        result, metadata = self.fake_capture(None)
        self.assertEqual(result, 1)
        self.assertEqual(metadata['status'], 'invalid-no-frames')

    def test_normal_window_close_is_not_a_crash(self):
        result, metadata = self.fake_capture('NATIVE_PRESENT frame=1\nSTOP window-closed @0x0: closed\n')
        self.assertEqual(result, 0)
        self.assertEqual(metadata['status'], 'captured-unverified')

    def test_capture_is_unverified_until_scene_and_feature_are_checked(self):
        result, metadata = self.fake_capture('NATIVE_GRAPHICS backend=Vulkan\nNATIVE_PRESENT frame=1\nSTOP window-closed @0x0: closed\n')
        self.assertEqual(result, 0)
        self.assertEqual(metadata['status'], 'captured-unverified')
        self.assertFalse(metadata['internal_resolution']['verified'])
        self.assertEqual(metadata['launcher_sfr_environment']['SFR_FRAME_METRICS'], '1')

    def test_capture_rejects_settings_changed_in_launcher(self):
        result, metadata = self.fake_capture('NATIVE_PRESENT frame=1\nSTOP window-closed @0x0: closed\n', change_settings=True)
        self.assertEqual(result, 1)
        self.assertEqual(metadata['status'], 'invalid-settings-change')

    def test_launcher_canonical_rewrite_preserves_equivalent_capture_settings(self):
        self.settings.write_text('audio=false\nfullscreen=true\nparallel=false\n'
                                 'window_width=01920\nwindow_height=00720\nvolume=00080\n')
        result, metadata = self.fake_capture(
            'NATIVE_PRESENT frame=1\nSTOP window-closed @0x0: closed\n',
            rewrite_settings={'audio': '0', 'fullscreen': '1', 'parallel': '0',
                              'window_width': '1920', 'window_height': '720', 'volume': '80'})
        self.assertEqual(result, 0)
        self.assertEqual(metadata['status'], 'captured-unverified')
        self.assertEqual(metadata['changed_settings'], {})

    def test_settings_normalization_only_accepts_launcher_supported_values(self):
        valid = self.runner.settings_values('audio=false\nfullscreen=true\nwindow_width=01920\nvolume=00080\n')
        self.assertEqual(valid, {'audio': '0', 'fullscreen': '1', 'window_width': '1920', 'volume': '80'})
        unsupported = {'audio': 'FALSE', 'window_width': '+1920', 'window_height': '001',
                       'volume': '101', 'player1_gamepad': '0001', 'render_scale': '051'}
        text = ''.join(f'{key}={value}\n' for key, value in unsupported.items())
        self.assertEqual(self.runner.settings_values(text), unsupported)


if __name__ == '__main__':
    unittest.main()
