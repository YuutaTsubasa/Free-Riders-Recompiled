"""Exercise the Windows runner with a tiny fake game; no game assets needed."""
from pathlib import Path
import json, os, shutil, subprocess, tempfile, unittest

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
Console.Error.WriteLine("STOP present-limit @0x0: fake");
Environment.Exit(3); } }''')
        script = Path(cls.build.name) / 'compile.ps1'
        script.write_text("Add-Type -Path '" + str(source) + "' -OutputAssembly '" + str(cls.exe) + "' -OutputType ConsoleApplication")
        subprocess.run([PS, '-NoProfile', '-File', str(script)], check=True, capture_output=True)
        with cls.exe.open('ab') as executable:
            executable.write(b'SFR_PRESENT_LIMIT_AFTER_SAY')

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

    def run_harness(self, *extra, stretch=False):
        env = dict(os.environ, SFR_HOSTILE_TEST='must-not-leak', SFR_REALTIME_RACE='0', SFR_CHECKPOINT_INTERVAL='999')
        env.pop('PSModulePath', None)  # Windows PowerShell must load its own modules, not pwsh's.
        return subprocess.run([PS, '-NoProfile', '-File', str(self.root / 'scripts/benchmark.ps1'),
            '-Configs', 'baseline', '-Repeats', '1', '-NoWarmup', '-SkipBuildCheck',
            *([] if stretch else ['-NoStretch']),
            '-Out', str(self.root / 'result'), *extra], cwd=self.root, env=env,
            capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=60)

    def test_child_isolated_normal_time_and_shared_data_preserved(self):
        p = self.run_harness('-ColdPipelines')
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        output = (self.root / 'result/baseline-1.out').read_text(encoding='utf-8-sig')
        self.assertNotIn('SFR_HOSTILE_TEST=', output)
        self.assertNotIn('SFR_CHECKPOINT_INTERVAL=999', output)
        for setting in ('SFR_REALTIME_RACE=1', 'SFR_REALTIME_UI=1', 'SFR_AUDIO=1', 'SFR_RENDER_EVERY=1'):
            self.assertIn(setting, output)
        self.assertEqual((self.root / 'pipeline-cache/keep').read_text(), 'keep-cache')
        self.assertEqual((self.root / 'out/build/host/save/progress').read_text(), 'keep-save')
        self.assertTrue((self.root / 'result/save-baseline-1/progress').exists())

    def test_existing_output_is_rejected_without_modification(self):
        (self.root / 'result').mkdir()
        (self.root / 'result/keep').write_text('existing')
        p = self.run_harness()
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual((self.root / 'result/keep').read_text(), 'existing')
        self.assertFalse((self.root / 'result/baseline-1.log').exists())

    def test_stretched_metadata_records_effective_stop_rule(self):
        p = self.run_harness(stretch=True)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        metadata = json.loads((self.root / 'result/baseline-1.json').read_text(encoding='utf-8-sig'))
        info = (self.root / 'result/info.txt').read_text(encoding='utf-8-sig')
        for name, key in [('present_limit', 'SFR_PRESENT_LIMIT'),
                          ('after_say', 'SFR_PRESENT_LIMIT_AFTER_SAY'),
                          ('reference_fps', 'SFR_SAY_REFERENCE_FPS')]:
            self.assertIn(f'{name}={metadata["settings"][key]}', info)

    def test_kit_preserves_existing_archive(self):
        archive = self.root / 'kit.zip'
        archive.write_bytes(b'keep archive')
        p = subprocess.run([PS, '-NoProfile', '-File', str(self.root / 'scripts/make_benchmark_kit.ps1'),
                            '-Destination', str(self.root / 'kit')], capture_output=True, timeout=30)
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual(archive.read_bytes(), b'keep archive')
        self.assertFalse((self.root / 'kit').exists())


if __name__ == '__main__':
    unittest.main()
