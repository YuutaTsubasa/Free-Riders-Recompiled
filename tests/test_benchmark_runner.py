"""Exercise the Windows runner with a tiny fake game; no game assets needed."""
from pathlib import Path
import hashlib, json, os, shutil, subprocess, tempfile, unittest

ROOT = Path(__file__).resolve().parents[1]
PS = shutil.which('powershell') if os.name == 'nt' else None


@unittest.skipUnless(PS, 'Windows PowerShell required')
class BenchmarkRunnerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.build.name) / 'fake.exe'
        source = Path(cls.build.name) / 'fake.cs'
        source.write_text('''using System; using System.IO;
class Fake { static void Main() {
foreach (System.Collections.DictionaryEntry e in Environment.GetEnvironmentVariables())
 if (e.Key.ToString().StartsWith("SFR_")) Console.WriteLine(e.Key + "=" + e.Value);
if (Environment.GetEnvironmentVariable("SFR_GAMEPLAY_METRICS") == "1") {
 var ci = System.Globalization.CultureInfo.InvariantCulture;
 for (int frame = 0; frame <= 5400; ++frame) {
  int tick = frame, cd = Math.Min(tick, 300), race = Math.Max(0, tick - 300);
  string state = tick == 0 ? "prestart" : "advancing";
  string stage = tick == 0 ? "none" : race == 0 ? "countdown" : "racing";
  int phase = tick == 0 ? 9 : tick < 300 ? 10 : 11;
  double remaining = Math.Max(0, 5000.0 - cd * (1000.0 / 60.0));
  double elapsed = race * (1000.0 / 60.0);
  Console.Error.WriteLine("NATIVE_PRESENT frame=" + frame + " gameplay_schema=1 game_contract=solo_fixed_v1 game_run=1 game_state=" + state + " game_stage=" + stage + " game_start_observed=" + (tick == 0 ? 0 : 1) + " game_phase=" + phase + " game_tick=" + tick + " game_countdown_tick=" + cd + " game_race_tick=" + race + " game_countdown_ms=" + remaining.ToString("F9", ci) + " game_elapsed_ms=" + elapsed.ToString("F9", ci) + " game_config=1:2:1:0:3:60:0 draw_timers=0 presented=1 frame_ms=10 racing=0 seconds=" + (frame / 60.0).ToString("F6", ci));
 }
 Console.Error.WriteLine(Environment.GetEnvironmentVariable("CODEX_FAKE_BAD_SEMANTIC_STOP") == "1" ? "STOP present-limit @0x0: legacy" : "STOP gameplay-limit @0x132: SFR_GAMEPLAY_TICK_LIMIT reached");
 Environment.Exit(3);
}
if (Environment.GetEnvironmentVariable("CODEX_FAKE_PREFIXED_PRESENT") == "1") {
 Console.Error.WriteLine("NATIVE_PRESENT frame=1 racing=1 seconds=0.02 ");
 Console.Error.WriteLine("RESULT NtCreateEvent type=0NATIVE_PRESENT frame=2 racing=1 seconds=0.04 ");
 Console.Error.WriteLine("NATIVE_PRESENT frame=3 racing=1 seconds=0.06 ");
}
if (Environment.GetEnvironmentVariable("CODEX_FAKE_MISMATCH") != null)
 Console.Error.WriteLine("STOP " + Environment.GetEnvironmentVariable("CODEX_FAKE_MISMATCH") + " @0x1234: fake");
Console.Error.WriteLine("STOP present-limit @0x0: fake");
Environment.Exit(3); } }''')
        script = Path(cls.build.name) / 'compile.ps1'
        script.write_text("Add-Type -Path '" + str(source) + "' -OutputAssembly '" + str(cls.exe) + "' -OutputType ConsoleApplication")
        subprocess.run([PS, '-NoProfile', '-File', str(script)], check=True, capture_output=True)
        with cls.exe.open('ab') as executable:
            executable.write(b'SFR_PRESENT_LIMIT_AFTER_SAY SFR_DRAW_TIMERS SFR_DIRECT_CONSTANT_UPLOAD SFR_MANAGED_STENCIL_STATE SFR_VERIFY_MANAGED_STENCIL_STATE SFR_GAMEPLAY_METRICS SFR_GAMEPLAY_TICK_LIMIT solo_fixed_v1')

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        shutil.copytree(ROOT / 'scripts', self.root / 'scripts', ignore=shutil.ignore_patterns('__pycache__'))
        host = self.root / 'out/build/host'
        host.mkdir(parents=True)
        shutil.copy2(self.exe, host / 'sfr_cpu_diagnostic.exe')
        for name in ('out/recomp/image-loader', 'private/assets', 'out/build/host/save', 'pipeline-cache'):
            (self.root / name).mkdir(parents=True, exist_ok=True)
        (host / 'save/progress').write_text('keep-save')
        (self.root / 'pipeline-cache/keep').write_text('keep-cache')

    def run_harness(self, *extra, stretch=False, configs='baseline', prefixed=False, mismatch=None,
                    warmup=False, semantic_fake=False, bad_semantic_stop=False, result_name='result'):
        env = dict(os.environ, SFR_HOSTILE_TEST='must-not-leak', SFR_REALTIME_RACE='0', SFR_CHECKPOINT_INTERVAL='999',
                   SFR_DRAW_TIMERS='1', SFR_MANAGED_TEXTURE_STATE='1', SFR_VERIFY_MANAGED_TEXTURE_STATE='1',
                   SFR_MANAGED_STENCIL_STATE='1', SFR_VERIFY_MANAGED_STENCIL_STATE='1',
                   SFR_UW_DEBUG='1', SFR_UW_ISOLATE='1', SFR_DRAW_SIGNATURE_TRACE='1',
                   SFR_TEXTURE_TIMERS='1', SFR_TEXTURE_TRACE='1',
                   SFR_PRESENT_LIMIT='9', SFR_PRESENT_LIMIT_RACING='8', SFR_PRESENT_LIMIT_AFTER_SAY='7',
                   SFR_GAMEPLAY_METRICS='0', SFR_GAMEPLAY_TICK_LIMIT='8', SFR_ASPECT_MODE='auto',
                   SFR_UI_ALIGN='center', SFR_RENDER_SCALE='50')
        env.pop('PSModulePath', None)  # Windows PowerShell must load its own modules, not pwsh's.
        if prefixed:
            env['CODEX_FAKE_PREFIXED_PRESENT'] = '1'
        if mismatch:
            env['CODEX_FAKE_MISMATCH'] = mismatch
        if semantic_fake:
            env['SFR_GAMEPLAY_METRICS'] = '1'
        if bad_semantic_stop:
            env['CODEX_FAKE_BAD_SEMANTIC_STOP'] = '1'
        no_warmup = [] if warmup else ['-NoWarmup']
        return subprocess.run([PS, '-NoProfile', '-File', str(self.root / 'scripts/benchmark.ps1'),
            '-Configs', configs, '-Repeats', '1', *no_warmup, '-SkipBuildCheck',
            *([] if stretch else ['-NoStretch']),
            '-Out', str(self.root / result_name), *extra], cwd=self.root, env=env,
            capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=60)

    def test_child_isolated_normal_time_and_shared_data_preserved(self):
        p = self.run_harness('-ColdPipelines')
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        output = (self.root / 'result/baseline-1.out').read_text(encoding='utf-8-sig')
        self.assertNotIn('SFR_HOSTILE_TEST=', output)
        self.assertNotIn('SFR_CHECKPOINT_INTERVAL=999', output)
        self.assertNotIn('SFR_MANAGED_TEXTURE_STATE=', output)
        self.assertNotIn('SFR_VERIFY_MANAGED_TEXTURE_STATE=', output)
        self.assertNotIn('SFR_MANAGED_STENCIL_STATE=', output)
        self.assertNotIn('SFR_VERIFY_MANAGED_STENCIL_STATE=', output)
        self.assertNotIn('SFR_UW_DEBUG=', output)
        self.assertNotIn('SFR_UW_ISOLATE=', output)
        self.assertNotIn('SFR_DRAW_SIGNATURE_TRACE=', output)
        self.assertNotIn('SFR_TEXTURE_TIMERS=', output)
        self.assertNotIn('SFR_TEXTURE_TRACE=', output)
        for setting in ('SFR_REALTIME_RACE=1', 'SFR_REALTIME_UI=1', 'SFR_AUDIO=1', 'SFR_RENDER_EVERY=1', 'SFR_FRAME_METRICS=1', 'SFR_DRAW_TIMERS=0'):
            self.assertIn(setting, output)
        self.assertEqual((self.root / 'pipeline-cache/keep').read_text(), 'keep-cache')
        self.assertEqual((self.root / 'out/build/host/save/progress').read_text(), 'keep-save')
        # The run had a copy of its own, removed once it ended; the fixture stays.
        self.assertIn('save-baseline-1', output)
        self.assertFalse((self.root / 'result/save-baseline-1').exists())
        self.assertTrue((self.root / 'result/fixture/progress').exists())
        metadata = json.loads((self.root / 'result/baseline-1.json').read_text(encoding='utf-8-sig'))
        self.assertEqual(metadata['sha256'].lower(), hashlib.sha256(self.exe.read_bytes()).hexdigest())
        fixture = json.loads((self.root / 'result/fixture-hashes.json').read_text(encoding='utf-8-sig'))
        if isinstance(fixture, dict):
            fixture = [fixture]
        self.assertEqual(fixture[0]['sha256'].lower(), hashlib.sha256(b'keep-save').hexdigest())

    def test_draw_timers_are_explicitly_opted_in(self):
        p = self.run_harness('-DetailedDrawMetrics')
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        output = (self.root / 'result/baseline-1.out').read_text(encoding='utf-8-sig')
        self.assertIn('SFR_DRAW_TIMERS=1', output)
        self.assertIn('SFR_TEXTURE_TIMERS=1', output)

    def test_strict_suite_stops_after_incomplete_run(self):
        p = self.run_harness('-StopOnFailure', '-NoShareArchive', configs='baseline,managed-buffers')
        self.assertNotEqual(p.returncode, 0)
        self.assertIn('Incomplete or failed run', p.stderr)
        self.assertFalse((self.root / 'result/managed-buffers-1.json').exists())

    def test_strict_suite_accepts_complete_prefixed_present(self):
        p = self.run_harness('-StopOnFailure', '-NoShareArchive', '-RaceFrames', '3', prefixed=True)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertTrue((self.root / 'result/summary.md').exists())

    def check_strict_mismatch(self, kind):
        p = self.run_harness('-StopOnFailure', '-NoShareArchive', '-RaceFrames', '3', prefixed=True,
                             configs='baseline,managed-buffers', mismatch=f'managed-{kind}-mismatch')
        self.assertNotEqual(p.returncode, 0)
        self.assertIn('Incomplete or failed run', p.stderr)
        self.assertFalse((self.root / 'result/managed-buffers-1.json').exists())

    def test_strict_suite_rejects_stencil_mismatch_before_expected_stop(self):
        self.check_strict_mismatch('stencil-state')

    def test_strict_suite_rejects_short_constant_mismatch_before_expected_stop(self):
        self.check_strict_mismatch('short-constants')

    def test_strict_suite_rejects_texture_mismatch_before_expected_stop(self):
        self.check_strict_mismatch('texture-state')

    def test_no_archive_still_generates_summary(self):
        p = self.run_harness('-NoShareArchive')
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertTrue((self.root / 'result/summary.md').exists())
        self.assertEqual(list(self.root.glob('result-shareable*.zip')), [])

    def test_old_executable_cannot_claim_low_overhead_mode(self):
        host = self.root / 'out/build/host/sfr_cpu_diagnostic.exe'
        host.write_bytes(host.read_bytes().replace(b'SFR_DRAW_TIMERS', b'OLD_DRAW_TIMER'))
        p = self.run_harness()
        self.assertNotEqual(p.returncode, 0)
        self.assertIn('cannot disable draw timers', p.stderr)
        self.assertFalse((self.root / 'result/baseline-1.out').exists())

    def test_codex_comparison_records_distinct_builds_and_only_enables_candidate(self):
        before = self.root / 'out/build/host/sfr_cpu_diagnostic_before.exe'
        before.write_bytes(self.exe.read_bytes() + b'BaselineSnapshot')
        p = self.run_harness(configs='codex-before,codex-control,codex-after')
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        old = json.loads((self.root / 'result/codex-before-1.json').read_text(encoding='utf-8-sig'))
        control = json.loads((self.root / 'result/codex-control-1.json').read_text(encoding='utf-8-sig'))
        new = json.loads((self.root / 'result/codex-after-1.json').read_text(encoding='utf-8-sig'))
        self.assertEqual(old['sha256'].lower(), hashlib.sha256(before.read_bytes()).hexdigest())
        self.assertEqual(new['sha256'].lower(), hashlib.sha256(self.exe.read_bytes()).hexdigest())
        self.assertNotIn('SFR_MANAGED_CONSTANTS', old['settings'])
        self.assertNotIn('SFR_MANAGED_TEXTURE_STATE', old['settings'])
        self.assertNotIn('SFR_MANAGED_TEXTURE_STATE', control['settings'])
        self.assertNotIn('SFR_MANAGED_STENCIL_STATE', old['settings'])
        self.assertNotIn('SFR_MANAGED_STENCIL_STATE', control['settings'])
        self.assertEqual(new['settings']['SFR_MANAGED_CONSTANTS'], '1')
        self.assertEqual(new['settings']['SFR_MANAGED_TEXTURE_STATE'], '1')
        self.assertEqual(new['settings']['SFR_MANAGED_STENCIL_STATE'], '1')
        for metadata in (old, control, new):
            self.assertEqual(metadata['settings']['SFR_DRAW_TIMERS'], '0')
            self.assertNotIn('SFR_VERIFY_MANAGED_CONSTANTS', metadata['settings'])
            self.assertNotIn('SFR_VERIFY_MANAGED_TEXTURE_STATE', metadata['settings'])
            self.assertNotIn('SFR_VERIFY_MANAGED_STENCIL_STATE', metadata['settings'])

    def test_candidate_modes_record_texture_flag_and_separate_verification(self):
        measured = ('managed-constants', 'managed-buffers', 'codex-after')
        verification = ('verify-constants', 'verify-buffers', 'resource-audit')
        p = self.run_harness('-NoShareArchive', configs=','.join(measured + verification))
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        for config in measured + verification:
            with self.subTest(config=config):
                settings = json.loads((self.root / f'result/{config}-1.json').read_text(encoding='utf-8-sig'))['settings']
                self.assertEqual(settings['SFR_MANAGED_TEXTURE_STATE'], '1')
                self.assertEqual(settings['SFR_MANAGED_STENCIL_STATE'], '1')
                self.assertEqual(settings['SFR_MANAGED_CONSTANTS'], '1')
                for key in ('SFR_VERIFY_MANAGED_TEXTURE_STATE', 'SFR_VERIFY_MANAGED_STENCIL_STATE', 'SFR_VERIFY_MANAGED_CONSTANTS'):
                    if config in verification:
                        self.assertEqual(settings[key], '1')
                    else:
                        self.assertNotIn(key, settings)

    def test_keep_saves_keeps_each_runs_copy(self):
        p = self.run_harness('-KeepSaves')
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertTrue((self.root / 'result/save-baseline-1/progress').exists())

    def test_direct_upload_comparison_keeps_default_and_verification_separate(self):
        configs = ('direct-buffers', 'managed-buffers', 'verify-direct', 'direct-constants')
        p = self.run_harness(configs=','.join(configs))
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        hashes = set()
        for config in configs:
            meta = json.loads((self.root / f'result/{config}-1.json').read_text(encoding='utf-8-sig'))
            hashes.add(meta['sha256'])
            settings = meta['settings']
            self.assertEqual(settings['SFR_MANAGED_CONSTANTS'], '1')
            self.assertEqual(settings['SFR_MANAGED_TEXTURE_STATE'], '1')
            self.assertEqual(settings['SFR_MANAGED_STENCIL_STATE'], '1')
            self.assertEqual(settings.get('SFR_DIRECT_CONSTANT_UPLOAD', '0'),
                             '0' if config == 'managed-buffers' else '1')
            self.assertEqual(settings.get('SFR_MANAGED_BUFFERS', '0'),
                             '0' if config == 'direct-constants' else '1')
            for key in ('SFR_VERIFY_MANAGED_CONSTANTS', 'SFR_VERIFY_MANAGED_BUFFERS',
                        'SFR_VERIFY_MANAGED_TEXTURE_STATE', 'SFR_VERIFY_MANAGED_STENCIL_STATE', 'SFR_D3D12_DRED'):
                self.assertEqual(settings.get(key, '0'), '1' if config == 'verify-direct' else '0')
            self.assertEqual(settings['SFR_DRAW_TIMERS'], '0')
        self.assertEqual(len(hashes), 1)

    def test_old_executable_cannot_claim_direct_upload_even_with_build_check_skipped(self):
        host = self.root / 'out/build/host/sfr_cpu_diagnostic.exe'
        host.write_bytes(host.read_bytes().replace(b'SFR_DIRECT_CONSTANT_UPLOAD', b'OLD_DIRECT_CONSTANT_UPLOAD'))
        p = self.run_harness(configs='managed-buffers,direct-buffers,verify-direct')
        self.assertNotEqual(p.returncode, 0)
        self.assertIn('does not support SFR_DIRECT_CONSTANT_UPLOAD', p.stderr)
        self.assertFalse((self.root / 'result').exists())

    def test_existing_output_is_rejected_without_modification(self):
        (self.root / 'result').mkdir()
        (self.root / 'result/keep').write_text('existing')
        p = self.run_harness()
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual((self.root / 'result/keep').read_text(), 'existing')
        self.assertFalse((self.root / 'result/baseline-1.log').exists())

    def test_old_executable_cannot_claim_stencil_publication(self):
        host = self.root / 'out/build/host/sfr_cpu_diagnostic.exe'
        host.write_bytes(host.read_bytes().replace(b'SFR_MANAGED_STENCIL_STATE', b'OLD_MANAGED_STENCIL_STATE'))
        p = self.run_harness(configs='codex-after')
        self.assertNotEqual(p.returncode, 0)
        self.assertIn('does not support SFR_MANAGED_STENCIL_STATE', p.stderr)
        self.assertFalse((self.root / 'result').exists())

    def test_executable_without_stencil_verifier_cannot_claim_verification(self):
        host = self.root / 'out/build/host/sfr_cpu_diagnostic.exe'
        host.write_bytes(host.read_bytes().replace(b'SFR_VERIFY_MANAGED_STENCIL_STATE', b'OLD_VERIFY_MANAGED_STENCIL_STATE'))
        p = self.run_harness(configs='verify-buffers')
        self.assertNotEqual(p.returncode, 0)
        self.assertIn('does not support SFR_VERIFY_MANAGED_STENCIL_STATE', p.stderr)
        self.assertFalse((self.root / 'result').exists())

    def test_stretched_metadata_records_effective_stop_rule(self):
        p = self.run_harness(stretch=True)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        metadata = json.loads((self.root / 'result/baseline-1.json').read_text(encoding='utf-8-sig'))
        info = (self.root / 'result/info.txt').read_text(encoding='utf-8-sig')
        for name, key in [('present_limit', 'SFR_PRESENT_LIMIT'),
                          ('after_say', 'SFR_PRESENT_LIMIT_AFTER_SAY'),
                          ('reference_fps', 'SFR_SAY_REFERENCE_FPS')]:
            self.assertIn(f'{name}={metadata["settings"][key]}', info)

    def test_race_limit_solo_and_fixed_step(self):
        # A build that counts the race's own presents gets that limit instead of the one after the last word.
        host = self.root / 'out/build/host/sfr_cpu_diagnostic.exe'
        with host.open('ab') as executable:
            executable.write(b'SFR_PRESENT_LIMIT_RACING')
        p = self.run_harness('-Scenario', 'solo', '-FixedStep', stretch=True)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        output = (self.root / 'result/baseline-1.out').read_text(encoding='utf-8-sig')
        self.assertIn('SFR_PRESENT_LIMIT_RACING=5400', output)
        self.assertNotIn('SFR_PRESENT_LIMIT_AFTER_SAY=', output)
        self.assertIn('SFR_REALTIME_RACE=0', output)
        self.assertIn('SFR_SAY=ok@580,ok@930,ok@1280,start@1630,ok@1830,right@2030,', output)
        info = (self.root / 'result/info.txt').read_text(encoding='utf-8-sig')
        self.assertIn('race_frames=5400 scenario=solo fixed_step=True', info)
        self.assertIn('realtime_race=0', info)

    def test_old_build_keeps_the_limit_after_the_last_word(self):
        p = self.run_harness('-RaceFrames', '3000', stretch=True)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        output = (self.root / 'result/baseline-1.out').read_text(encoding='utf-8-sig')
        self.assertIn('SFR_PRESENT_LIMIT_AFTER_SAY=4200', output)
        self.assertNotIn('SFR_PRESENT_LIMIT_RACING=', output)
        self.assertIn('scenario=race fixed_step=False', (self.root / 'result/info.txt').read_text(encoding='utf-8-sig'))

    def semantic_args(self):
        return ('-GameplayTicks', '5400', '-Scenario', 'solo', '-FixedStep')

    def test_semantic_mode_rejects_conflicting_benchmark_options_before_launch(self):
        invalid = [('-Scenario', 'race'), ('-NoStretch',), ('-RaceFrames', '1'),
                   ('-PresentLimit', '10'), ('-AfterSay', '10'), ('-Capped',),
                   ('-NoAudio',), ('-DetailedDrawMetrics',), ('-AllowDiagnosticRendering',)]
        for index, option in enumerate(invalid):
            with self.subTest(option=option):
                p = self.run_harness(*self.semantic_args(), *option, stretch=True,
                                     configs='semantic-before', semantic_fake=True,
                                     result_name=f'invalid-option-{index}')
                self.assertNotEqual(p.returncode, 0, p.stdout + p.stderr)
                self.assertFalse((self.root / f'invalid-option-{index}/semantic-before-1.out').exists())

    def test_semantic_mode_accepts_only_its_four_configurations(self):
        for index, config in enumerate(('baseline', 'managed-buffers', 'no-audio')):
            with self.subTest(config=config):
                p = self.run_harness(*self.semantic_args(), stretch=True, configs=config,
                                     semantic_fake=True, result_name=f'invalid-config-{index}')
                self.assertNotEqual(p.returncode, 0, p.stdout + p.stderr)
                self.assertFalse((self.root / f'invalid-config-{index}/{config}-1.out').exists())

    def test_semantic_capabilities_are_checked_before_launch(self):
        host = self.root / 'out/build/host/sfr_cpu_diagnostic.exe'
        before = self.root / 'out/build/host/sfr_cpu_diagnostic_before.exe'
        for index, capability in enumerate((b'SFR_GAMEPLAY_METRICS', b'SFR_GAMEPLAY_TICK_LIMIT', b'solo_fixed_v1')):
            with self.subTest(capability=capability):
                original = host.read_bytes()
                missing = original.replace(capability, b'X' * len(capability), 1)
                host.write_bytes(missing)
                before.write_bytes(missing + b'SemanticBaselineSnapshot')
                p = self.run_harness(*self.semantic_args(), stretch=True, configs='semantic-before,semantic-after',
                                     semantic_fake=True, result_name=f'missing-capability-{index}')
                self.assertNotEqual(p.returncode, 0, p.stdout + p.stderr)
                self.assertIn('lacks supported semantic gameplay observer', p.stderr)
                self.assertFalse((self.root / f'missing-capability-{index}/semantic-before-1.out').exists())
                self.assertFalse((self.root / f'missing-capability-{index}/semantic-after-1.out').exists())
                host.write_bytes(original)

    def test_semantic_suite_validates_ticks_and_uses_required_settings(self):
        before = self.root / 'out/build/host/sfr_cpu_diagnostic_before.exe'
        before.write_bytes(self.exe.read_bytes() + b'SemanticBaselineSnapshot')
        configs = 'semantic-before,semantic-control,semantic-after,semantic-verify'
        p = self.run_harness(*self.semantic_args(), '-StopOnFailure', '-NoShareArchive',
                             stretch=True, configs=configs, semantic_fake=True)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertFalse((self.root / 'result/summary.md').exists())
        for config in configs.split(','):
            metadata = json.loads((self.root / f'result/{config}-1.json').read_text(encoding='utf-8-sig'))
            settings = metadata['settings']
            for name, value in (('SFR_GAMEPLAY_METRICS', '1'), ('SFR_GAMEPLAY_TICK_LIMIT', '5400'),
                                ('SFR_PRESENT_LIMIT', '0'), ('SFR_PRESENT_LIMIT_RACING', '0'),
                                ('SFR_PRESENT_LIMIT_AFTER_SAY', '0'), ('SFR_FRAME_LIMIT', '0'),
                                ('SFR_FRAME_METRICS', '1'), ('SFR_DRAW_TIMERS', '0'),
                                ('SFR_REALTIME_RACE', '0'), ('SFR_AUDIO', '1'),
                                ('SFR_ASPECT_MODE', 'original'),
                                ('SFR_UI_ALIGN', 'edge'), ('SFR_RENDER_SCALE', '100'),
                                ('SFR_TEXTURE_TIMERS', '0'), ('SFR_TEXTURE_TRACE', '0'),
                                ('SFR_DRAW_SIGNATURE_TRACE', '0'), ('SFR_MANAGED_CONSTANTS', '1'),
                                ('SFR_MANAGED_TEXTURE_STATE', '1'), ('SFR_MANAGED_STENCIL_STATE', '1')):
                self.assertEqual(settings.get(name), value, f'{config}: {name}')
            self.assertEqual(settings.get('SFR_MANAGED_BUFFERS', '0'), '0')
        before = json.loads((self.root / 'result/semantic-before-1.json').read_text(encoding='utf-8-sig'))
        after = json.loads((self.root / 'result/semantic-after-1.json').read_text(encoding='utf-8-sig'))
        self.assertNotEqual(before['sha256'], after['sha256'])

    def test_semantic_warmup_uses_first_configuration_executable_and_tick_limit(self):
        before = self.root / 'out/build/host/sfr_cpu_diagnostic_before.exe'
        before.write_bytes(self.exe.read_bytes() + b'SemanticBaselineSnapshot')
        p = self.run_harness(*self.semantic_args(), stretch=True, configs='semantic-before,semantic-after',
                             semantic_fake=True, warmup=True)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        warm = json.loads((self.root / 'result/warmup-1.json').read_text(encoding='utf-8-sig'))
        first = json.loads((self.root / 'result/semantic-before-1.json').read_text(encoding='utf-8-sig'))
        self.assertEqual(warm['executable'], first['executable'])
        self.assertEqual(warm['settings']['SFR_GAMEPLAY_TICK_LIMIT'], '5400')
        self.assertEqual(warm['settings']['SFR_PRESENT_LIMIT'], '0')
        self.assertEqual(warm['settings']['SFR_PRESENT_LIMIT_RACING'], '0')
        self.assertEqual(warm['settings']['SFR_PRESENT_LIMIT_AFTER_SAY'], '0')

    def test_semantic_stop_requires_semantic_tick_limit_not_legacy_racing_count(self):
        before = self.root / 'out/build/host/sfr_cpu_diagnostic_before.exe'
        before.write_bytes(self.exe.read_bytes() + b'SemanticBaselineSnapshot')
        p = self.run_harness(*self.semantic_args(), '-StopOnFailure', stretch=True,
                             configs='semantic-before', semantic_fake=True, bad_semantic_stop=True)
        self.assertNotEqual(p.returncode, 0)
        log = (self.root / 'result/semantic-before-1.log').read_text(encoding='utf-8-sig')
        self.assertIn('STOP present-limit', log)

    def test_kit_preserves_existing_archive(self):
        archive = self.root / 'kit.zip'
        archive.write_bytes(b'keep archive')
        p = subprocess.run([PS, '-NoProfile', '-File', str(self.root / 'scripts/make_benchmark_kit.ps1'),
                            '-Destination', str(self.root / 'kit')], capture_output=True, timeout=30)
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual(archive.read_bytes(), b'keep archive')
        self.assertFalse((self.root / 'kit').exists())

    def test_kit_copies_frame_and_gameplay_validators(self):
        kit = self.root / 'updated-kit'
        kit.mkdir()
        p = subprocess.run([PS, '-NoProfile', '-File', str(self.root / 'scripts/make_benchmark_kit.ps1'),
                            '-Destination', str(kit), '-UpdateOnly'], capture_output=True, timeout=30)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        for name in ('benchmark_frames.py', 'benchmark_gameplay.py'):
            self.assertEqual((kit / 'scripts' / name).read_bytes(), (self.root / 'scripts' / name).read_bytes())


if __name__ == '__main__':
    unittest.main()
