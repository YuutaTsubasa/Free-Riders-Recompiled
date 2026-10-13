"""Exercise real PowerShell resume staging with synthetic logs and inert binaries."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import unittest

import test_benchmark_gameplay_i5 as gameplay_fixtures


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


class ResumeWrapperTest(unittest.TestCase):
    def setUp(self):
        # Reuse the established full 5400-tick fixture and ordinary workspace
        # UUID directories, avoiding restrictive tempfile ACLs on Windows.
        gameplay_fixtures.SemanticSuiteTest.setUpClass()
        self.fixture = gameplay_fixtures.SemanticSuiteTest(methodName='test_resume_exact_boundary_is_read_only_and_normal_report_still_invalid')
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.root = self.fixture.root
        self.repo = Path(__file__).resolve().parents[1]
        self.local = self.root / 'sessions'
        self.source = self.local / 'original-session'
        self.source_results = self.source / 'results'
        self.source_results.mkdir(parents=True)
        self.exports = self.root / 'exports'
        self.kit = self.root / 'kit'
        self.kit.mkdir()
        self.files = []
        for name in ('benchmark_codex_i5.ps1', 'benchmark_codex_gameplay_report.py',
                     'benchmark_gameplay.py', 'benchmark_summary.py'):
            self.package_file('scripts/' + name, (self.repo / 'scripts' / name).read_bytes())
        self.package_file('scripts/benchmark.ps1', b"throw 'Unexpected benchmark launch during PrepareOnly'\r\n")
        self.package_file('out/build/host/sfr_cpu_diagnostic.exe', b'INERT CANDIDATE - NEVER EXECUTE')
        self.package_file('out/build/host/sfr_cpu_diagnostic_before.exe', b'INERT BASELINE - NEVER EXECUTE')
        self.package_file('out/build/host/save/E000/profile.bin', b'SYNTHETIC SAVE FIXTURE')
        image, assets = self.root / 'image', self.root / 'assets'
        image.mkdir()
        assets.mkdir()
        self.package_file('play/settings.ini', f'image_directory={image}\nasset_directory={assets}\n'.encode())
        self.manifest = dict(files=self.files, candidate_commit='f' * 40,
                             baseline_commit='d' * 40, candidate_runtime_commit='e' * 40,
                             candidate_sha256=sha256(self.kit / 'out/build/host/sfr_cpu_diagnostic.exe'),
                             baseline_sha256=sha256(self.kit / 'out/build/host/sfr_cpu_diagnostic_before.exe'))
        self.save_manifest()
        self.fixture.root = self.source_results
        self.fixture.suite.update({key: self.manifest[key] for key in (
            'candidate_commit', 'baseline_commit', 'candidate_runtime_commit',
            'candidate_sha256', 'baseline_sha256')})
        self.fixture.suite['fixture_files'] = [dict(path='E000/profile.bin',
            sha256=sha256(self.kit / 'out/build/host/save/E000/profile.bin'))]
        self.fixture.write_resume_boundary()
        # Include original times/context alongside all five log/metadata pairs.
        for stage in ('verify', 'control'):
            (self.source_results / stage / 'info.txt').write_text(
                'power=no battery (desktop) plan=High performance idle_cpu_percent=8\n', encoding='utf-8')
            (self.source_results / stage / 'fixture-hashes.json').write_text(json.dumps(
                self.fixture.suite['fixture_files']), encoding='utf-8-sig')

    def package_file(self, relative, content):
        path = self.kit / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        self.files.append(dict(path=relative, sha256=sha256(path)))

    def save_manifest(self):
        (self.kit / 'manifest.json').write_text(json.dumps(self.manifest), encoding='utf-8-sig')

    def snapshot(self):
        return {str(p.relative_to(self.source)): (sha256(p), p.stat().st_mtime_ns)
                for p in self.source.rglob('*') if p.is_file()}

    def prepare(self):
        command = [shutil.which('powershell') or shutil.which('pwsh'), '-NoProfile',
                   '-ExecutionPolicy', 'Bypass', '-File', str(self.kit / 'scripts/benchmark_codex_i5.ps1'),
                   '-PrepareOnly', '-GameplayTicks', '5400', '-ResumeFrom', str(self.source),
                   '-LocalRoot', str(self.local), '-ResultsRoot', str(self.exports)]
        if not command[0]:
            self.skipTest('PowerShell is unavailable')
        return subprocess.run(command, text=True, encoding='utf-8', errors='replace',
                              capture_output=True, timeout=90, check=False)

    def new_result(self):
        fresh = [p for p in self.local.iterdir() if p != self.source]
        self.assertEqual(len(fresh), 1)
        return fresh[0] / 'results'

    def test_prepare_only_copies_passed_evidence_with_provenance_and_no_source_writes(self):
        before = self.snapshot()
        prepared = self.prepare()
        self.assertEqual(prepared.returncode, 0, prepared.stdout + prepared.stderr)
        self.assertEqual(self.snapshot(), before)
        result = self.new_result()
        state = json.loads((result / 'suite.json').read_text(encoding='utf-8-sig'))
        self.assertEqual(state['status'], 'prepared')
        self.assertEqual(Path(state['resumed_from']), self.source)
        self.assertEqual(state['resume_source_suite'], self.fixture.suite)
        self.assertEqual(state['stages'], self.fixture.suite['stages'])
        self.assertEqual(state['preflight'], self.fixture.suite['preflight'])
        self.assertFalse((result / 'compare').exists())
        self.assertEqual(len(list(result.glob('*/*.log'))), 5)
        inventory = {item['path'].replace('\\', '/'): item['sha256'] for item in state['import_inventory']}
        expected = {}
        for stage in ('verify', 'control'):
            for original in (self.source_results / stage).rglob('*'):
                if original.is_file():
                    relative = str(original.relative_to(self.source_results)).replace('\\', '/')
                    expected[relative] = sha256(original)
                    self.assertEqual(sha256(result / relative), sha256(original))
        self.assertEqual(inventory, expected)
        self.assertTrue((self.exports / result.parent.name / 'suite.json').is_file())

    def test_prepare_only_rejects_partial_comparison_without_source_writes(self):
        (self.source_results / 'compare').mkdir()
        before = self.snapshot()
        prepared = self.prepare()
        self.assertNotEqual(prepared.returncode, 0)
        self.assertEqual(self.snapshot(), before)
        self.assertIn('resume integrity failed', prepared.stdout + prepared.stderr)

    def test_prepare_only_rejects_manifest_parameter_mismatch(self):
        self.manifest['candidate_runtime_commit'] = '1' * 40
        self.save_manifest()
        before = self.snapshot()
        prepared = self.prepare()
        self.assertNotEqual(prepared.returncode, 0)
        self.assertEqual(self.snapshot(), before)
        self.assertIn('Resume parameter differs: candidate_runtime_commit', prepared.stdout + prepared.stderr)

    def test_prepare_only_rejects_changed_staged_binary_even_with_valid_file_entry_hash(self):
        binary = self.kit / 'out/build/host/sfr_cpu_diagnostic.exe'
        binary.write_bytes(b'CHANGED INERT CANDIDATE - NEVER EXECUTE')
        for entry in self.files:
            if entry['path'] == 'out/build/host/sfr_cpu_diagnostic.exe':
                entry['sha256'] = sha256(binary)
        self.save_manifest()
        before = self.snapshot()
        prepared = self.prepare()
        self.assertNotEqual(prepared.returncode, 0, 'Changed actual staged binary must not reuse old verification')
        self.assertEqual(self.snapshot(), before)

    def test_prepare_only_rejects_changed_save_fixture(self):
        save = self.kit / 'out/build/host/save/E000/profile.bin'
        save.write_bytes(b'CHANGED SYNTHETIC SAVE')
        for entry in self.files:
            if entry['path'] == 'out/build/host/save/E000/profile.bin':
                entry['sha256'] = sha256(save)
        self.save_manifest()
        before = self.snapshot()
        prepared = self.prepare()
        self.assertNotEqual(prepared.returncode, 0)
        self.assertEqual(self.snapshot(), before)
        self.assertIn('Resume save fixture differs', prepared.stdout + prepared.stderr)


if __name__ == '__main__':
    unittest.main()
