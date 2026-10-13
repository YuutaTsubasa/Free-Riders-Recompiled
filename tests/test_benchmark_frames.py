import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from benchmark_frames import summarize, scene_anchor


class BenchmarkFramesTests(unittest.TestCase):
    def test_scene_anchor_excludes_menu_and_requires_matching_threshold(self):
        lines = ['NATIVE_PRESENT frame=42 draws=600', 'NATIVE_PRESENT frame=51 draws=601']
        self.assertEqual(scene_anchor(lines, 600), 51)
        with self.assertRaisesRegex(ValueError, 'threshold'):
            scene_anchor(lines, 601)

    def test_frame_intervals_and_pacing_are_separate(self):
        lines = [
            'NATIVE_PRESENT frame=10 seconds=1.000 draws=800 pipeline_ms=0',
            'NATIVE_PRESENT frame=11 seconds=1.016 draws=800 frame_ms=16 pacing_ms=6 pipeline_ms=0',
            'NATIVE_PRESENT frame=12 seconds=1.040 draws=800 frame_ms=24 pacing_ms=4 pipeline_ms=2',
        ]
        r = summarize(lines, 10, 12)
        self.assertEqual(r['intervals'], 2)
        self.assertAlmostEqual(r['fps'], 50)
        self.assertAlmostEqual(r['frame_ms']['p95'], 24)
        self.assertAlmostEqual(r['non_pacing_ms']['mean'], 15)
        self.assertEqual(r['pipeline_compile_frames'], 1)

    def test_does_not_bridge_gaps_or_include_other_scenes(self):
        lines = [f'NATIVE_PRESENT frame={f} seconds={s} draws=800'
                 for f, s in [(9, 0), (10, 1), (11, 1.02), (13, 1.06), (14, 1.08), (15, 10)]]
        r = summarize(lines, 10, 14)
        self.assertEqual(r['intervals'], 2)
        self.assertEqual(r['missing_intervals'], 1)
        self.assertAlmostEqual(r['fps'], 50)
        self.assertIsNone(r['non_pacing_ms'])

    def test_legacy_seconds_and_no_data(self):
        with self.assertRaisesRegex(ValueError, 'consecutive'):
            summarize(['NATIVE_PRESENT frame=1 seconds=1 draws=1'], 1, 2)
        with self.assertRaisesRegex(ValueError, 'positive'):
            summarize(['NATIVE_PRESENT frame=1 seconds=1 draws=1',
                       'NATIVE_PRESENT frame=2 seconds=0 draws=1'], 1, 2)

    def test_texture_time_is_available_for_legacy_and_enabled_logs(self):
        for switch in ('', ' texture_timers=1'):
            with self.subTest(switch=switch):
                lines = [f'NATIVE_PRESENT frame=1 seconds=1.000 draws=1 texture_ms=2',
                         f'NATIVE_PRESENT frame=2 seconds=1.020 draws=1{switch} texture_ms=4']
                result = summarize(lines, 1, 2)
                self.assertIs(result['texture_timers'], True)
                self.assertEqual(result['metrics']['texture_ms']['mean'], 4)

    def test_disabled_and_mixed_texture_timers_are_unavailable(self):
        cases = [([' texture_timers=0 texture_ms=0'], False),
                 ([' texture_timers=1 texture_ms=3', ' texture_timers=0 texture_ms=0'], 'mixed')]
        for rows, availability in cases:
            with self.subTest(availability=availability):
                lines = ['NATIVE_PRESENT frame=1 seconds=1.000 draws=1 texture_ms=2']
                for frame, fields in enumerate(rows, 2):
                    lines.append(f'NATIVE_PRESENT frame={frame} seconds={1 + .02 * (frame - 1):.3f} draws=1{fields}')
                result = summarize(lines, 1, len(lines))
                self.assertEqual(result['texture_timers'], availability)
                self.assertIsNone(result['metrics']['texture_ms'])
                self.assertAlmostEqual(result['fps'], 50)

    def test_rejects_unknown_texture_timer_state_and_missing_texture_sample(self):
        with self.assertRaisesRegex(ValueError, 'invalid texture_timers'):
            summarize(['NATIVE_PRESENT frame=1 seconds=1.000 draws=1 texture_ms=2',
                       'NATIVE_PRESENT frame=2 seconds=1.020 draws=1 texture_timers=maybe texture_ms=4'], 1, 2)
        result = summarize(['NATIVE_PRESENT frame=1 seconds=1.000 draws=1 texture_ms=2',
                            'NATIVE_PRESENT frame=2 seconds=1.020 draws=1 texture_timers=1 texture_ms=4',
                            'NATIVE_PRESENT frame=3 seconds=1.040 draws=1 texture_timers=1'], 1, 3)
        self.assertIs(result['texture_timers'], True)
        self.assertIsNone(result['metrics']['texture_ms'])


if __name__ == '__main__':
    unittest.main()
