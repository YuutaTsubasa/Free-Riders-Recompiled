"""Offline context review must not turn completeness into performance acceptance."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from benchmark_codex_i5_quality import audit_quality, write_audit


class I5QualityTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.suite = dict(status='completed', repeats=6, control_repeats=2)
        self.measurements = dict(valid=True, runs=[
            dict(stage=stage, config=config, repeat=n, errors=[], fps=40.0)
            for stage, configs in [('verify', ['verify-buffers']), ('control', ['codex-before', 'codex-control']),
                                    ('compare', ['codex-before', 'codex-after', 'managed-buffers'])]
            for config in configs
            for n in range(1, (1 if stage == 'verify' else self.suite[
                'control_repeats' if stage == 'control' else 'repeats']) + 1)])
        self.save_integrity()
        for stage in ('control', 'compare'):
            (self.root / stage).mkdir()
            self.context(stage, 0)
        self.notes([])

    def save_integrity(self):
        for name, value in [('suite.json', self.suite), ('measurements.json', self.measurements)]:
            (self.root / name).write_text(json.dumps(value), encoding='utf-8-sig')

    def context(self, stage, cpu, power='no battery (desktop)'):
        (self.root / stage / 'info.txt').write_text(
            f'cpu=i5-3470\npower={power} plan=High performance idle_cpu_percent={cpu}\n'
            f'power={power} plan=High performance\n', encoding='utf-8-sig')

    def notes(self, observations):
        (self.root / 'measurement-notes.json').write_text(json.dumps(dict(observations=observations)))

    def test_complete_data_with_observed_load_and_activity_requires_review(self):
        self.context('compare', 25)
        self.notes([dict(stage='compare', rounds=[1], detail='Read-only observer queried result paths during round 1')])
        result = audit_quality(self.root)
        self.assertTrue(result['integrity_valid'])
        self.assertEqual(result['quality_status'], 'review_required')
        self.assertEqual(result['context']['compare']['admission_cpu_percent'], 25)
        self.assertEqual(result['observations'][0]['rounds'], [1])
        self.assertIn('not a measurement of load throughout', next(
            f['detail'] for f in result['findings'] if f['code'] == 'admission_cpu_load'))

    def test_audit_does_not_modify_performance_or_input_files(self):
        protected = [p for p in self.root.rglob('*') if p.is_file()]
        before = {p: p.read_bytes() for p in protected}
        result = write_audit(self.root)
        self.assertEqual(result['quality_status'], 'no_recorded_context_issue')
        self.assertEqual({p: p.read_bytes() for p in protected}, before)
        self.assertEqual(len(self.measurements['runs']), 23)
        self.assertIn('NOT displayed stable-60-Hz acceptance', (self.root / 'quality.md').read_text())

    def test_missing_context_and_observer_record_are_unknown(self):
        (self.root / 'compare/info.txt').unlink()
        (self.root / 'measurement-notes.json').unlink()
        result = audit_quality(self.root)
        self.assertTrue(result['integrity_valid'])
        self.assertEqual(result['quality_status'], 'unknown')
        self.assertEqual(result['observer_record'], 'unknown')
        self.assertEqual(result['context']['compare']['status'], 'unknown')

    def test_battery_heuristic_does_not_claim_actual_power_or_throttling(self):
        self.context('compare', 0, power='ON BATTERY')
        result = write_audit(self.root)
        self.assertEqual(result['context']['compare']['power_label'], 'ON BATTERY')
        warning = next(f for f in result['findings'] if f['code'] == 'unverified_power_label')
        self.assertIn('actual AC state', warning['detail'])
        self.assertIn('unverified', warning['detail'])

    def test_unknown_power_or_plan_is_not_reported_as_issue_free(self):
        for power, plan in [('unknown', 'High performance'), ('no battery (desktop)', 'unknown'), ('', '')]:
            with self.subTest(power=power, plan=plan):
                (self.root / 'compare/info.txt').write_text(
                    f'power={power} plan={plan} idle_cpu_percent=0\n')
                result = audit_quality(self.root)
                self.assertTrue(result['integrity_valid'])
                self.assertEqual(result['quality_status'], 'unknown')

    def test_missing_duplicate_unexpected_and_failed_runs_are_not_complete(self):
        original = list(self.measurements['runs'])
        variants = [original[:-1], original + [original[-1]],
                    original + [dict(stage='compare', config='unknown', repeat=1, errors=[])],
                    [dict(run, errors=['mismatch']) if n == 0 else run for n, run in enumerate(original)]]
        for runs in variants:
            with self.subTest(runs=len(runs)):
                self.measurements['runs'] = runs
                self.save_integrity()
                result = audit_quality(self.root)
                self.assertFalse(result['integrity_valid'])
                self.assertEqual(result['quality_status'], 'incomplete')

    def test_failed_original_verdict_and_interrupted_suite_stay_failed(self):
        for status, valid in [('failed', True), ('completed', False), ('running', True)]:
            with self.subTest(status=status, valid=valid):
                self.suite['status'] = status
                self.measurements['valid'] = valid
                self.save_integrity()
                self.assertFalse(audit_quality(self.root)['integrity_valid'])

    def test_ambiguous_or_invalid_admission_context_is_unknown(self):
        path = self.root / 'compare/info.txt'
        for content in ['power=no battery (desktop) plan=High performance',
                        'power=on mains plan=High performance idle_cpu_percent=101',
                        'power=on mains plan=High performance idle_cpu_percent=0\n' * 2]:
            with self.subTest(content=content):
                path.write_text(content)
                self.assertEqual(audit_quality(self.root)['quality_status'], 'unknown')

    def test_invalid_observer_scope_is_reported_and_never_excludes_a_run(self):
        for note in [dict(stage='compare', rounds=[7], detail='out of range'),
                     dict(stage='suite', rounds=[1], detail='ambiguous'),
                     dict(stage='compare', rounds=[True], detail='invalid identity')]:
            with self.subTest(note=note):
                self.notes([note])
                result = audit_quality(self.root)
                self.assertTrue(result['integrity_valid'])
                self.assertEqual(result['observer_record'], 'unknown')
                self.assertEqual(result['quality_status'], 'unknown')

    def test_malformed_inputs_fail_closed_without_changing_original_verdict(self):
        (self.root / 'suite.json').write_text('{malformed')
        result = write_audit(self.root)
        self.assertFalse(result['integrity_valid'])
        self.assertTrue(json.loads((self.root / 'measurements.json').read_text(encoding='utf-8-sig'))['valid'])


if __name__ == '__main__':
    unittest.main()
