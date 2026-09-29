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


if __name__ == '__main__':
    unittest.main()
