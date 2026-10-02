from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import benchmark_summary as bench


def present(frame, seconds, racing, **fields):
    extra = ' '.join(f'{key}={value}' for key, value in fields.items())
    return (f'NATIVE_PRESENT source=0x824e65a0 device=0x71600000 lr=0x8249bc70 frame={frame} draws=800 {extra} '
            f'holders=1:20.00,7:3.00 racing={racing} presented=1 seconds={seconds:.3f}\n')


def write_log(directory, name, frame_ms, menu=3, race=6, stop='STOP present-limit @0x0: SFR_PRESENT_LIMIT reached'):
    lines, seconds = [], 0.0
    for frame in range(menu + race):
        racing = int(frame >= menu)
        seconds += (frame_ms if racing else 16) / 1000
        lines.append(present(frame, seconds, racing, draw_ms=5, main_queued_ms=2))
    if stop:
        lines.append(stop + '\n')
    (Path(directory) / name).write_text(''.join(lines), encoding='utf-8')


class BenchmarkSummaryTest(unittest.TestCase):
    def test_counts_only_race_frames_after_the_skipped_ones(self):
        with tempfile.TemporaryDirectory() as directory:
            write_log(directory, 'baseline-1.log', 40)
            frames, seen, ended = bench.read_run(Path(directory) / 'baseline-1.log', skip=2)
            self.assertEqual(seen, 6)
            self.assertEqual(len(frames), 4)
            self.assertTrue(all(abs(f['ms'] - 40) < 1e-6 for f in frames))
            self.assertEqual(frames[0]['main_held_ms'], 20.0)
            self.assertEqual(ended, 'present-limit')

    def test_the_first_race_frame_uses_the_gap_from_the_menu(self):
        with tempfile.TemporaryDirectory() as directory:
            write_log(directory, 'baseline-1.log', 40, menu=1, race=2)
            frames, _, _ = bench.read_run(Path(directory) / 'baseline-1.log', skip=0)
            self.assertEqual(len(frames), 2)

    def test_a_run_that_never_raced_or_stopped_is_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            write_log(directory, 'serial-1.log', 40, race=0, stop='')
            table, rows = bench.summarise(directory, skip=0)
            self.assertIsNone(rows[0][2])
            self.assertIn('no STOP line', table)

    def test_compares_each_setting_with_baseline(self):
        with tempfile.TemporaryDirectory() as directory:
            write_log(directory, 'warmup-1.log', 50)
            write_log(directory, 'baseline-1.log', 40)
            write_log(directory, 'baseline-2.log', 40)
            write_log(directory, 'skip-draws-1.log', 20)
            table, rows = bench.summarise(directory, skip=0)
            self.assertEqual(sorted({row[0] for row in rows}), ['baseline', 'skip-draws', 'warmup'])
            self.assertIn('| baseline | 2 | 25.0 | 40.0 | 40.0 | 0 | +0% |', table)
            self.assertIn('| skip-draws | 1 | 50.0 | 20.0 | 20.0 | 0 | +100% |', table)
            # The warm-up is listed with the runs but not compared.
            runs, comparison = table.split('\n\n', 1)
            self.assertIn('| warmup | 1 | 20.0', runs)
            self.assertNotIn('warmup', comparison)

    def test_a_setting_is_called_faster_only_when_most_rounds_agree_and_it_beats_the_noise(self):
        with tempfile.TemporaryDirectory() as directory:
            for repeat, (base, fast, noisy) in enumerate([(40, 30, 40), (41, 31, 36), (39, 29, 44), (40, 30, 38), (40, 30, 42)], 1):
                write_log(directory, f'baseline-{repeat}.log', base)
                write_log(directory, f'fast-{repeat}.log', fast)
                write_log(directory, f'noisy-{repeat}.log', noisy)
            table, _ = bench.summarise(directory, skip=0)
            paired = [line for line in table.splitlines() if '/5 |' in line]
            fast_row = next(line for line in paired if line.startswith('| fast |'))
            noisy_row = next(line for line in paired if line.startswith('| noisy |'))
            self.assertIn('較快', fast_row)
            self.assertIn('5/5', fast_row)
            self.assertIn('不能判定', noisy_row)

    def test_too_few_rounds_prove_nothing(self):
        with tempfile.TemporaryDirectory() as directory:
            for repeat in (1, 2):
                write_log(directory, f'baseline-{repeat}.log', 40)
                write_log(directory, f'fast-{repeat}.log', 20)
            table, _ = bench.summarise(directory, skip=0)
            self.assertIn('輪數不足', table)

    def test_a_run_that_did_not_reach_the_limit_or_raced_less_is_left_out_of_the_comparison(self):
        with tempfile.TemporaryDirectory() as directory:
            for repeat in range(1, 5):
                write_log(directory, f'baseline-{repeat}.log', 40, race=100)
            write_log(directory, 'crashed-1.log', 40, race=100, stop='STOP native-draw @0x7e: boom')
            write_log(directory, 'short-1.log', 40, race=60)
            table, _ = bench.summarise(directory, skip=0)
            self.assertIn('以下幾趟不納入比較', table)
            self.assertIn('crashed-1', table.split('以下幾趟不納入比較')[1])
            self.assertIn('short-1', table.split('以下幾趟不納入比較')[1])
            comparison = table.split('\n\n', 1)[1]
            self.assertNotIn('| crashed |', comparison)

    def test_long_frames_and_compile_hitches_are_counted(self):
        with tempfile.TemporaryDirectory() as directory:
            lines, seconds = [], 0.0
            for frame, (ms, pipeline_ms) in enumerate([(16, 0), (40, 0), (60, 0), (90, 0), (150, 80), (20, 3)]):
                seconds += ms / 1000
                lines.append(present(frame, seconds, 1, pipelines=2 if pipeline_ms else 0, pipeline_ms=pipeline_ms))
            (Path(directory) / 'baseline-1.log').write_text(''.join(lines) + 'STOP present-limit @0x0: x\n', encoding='utf-8')
            frames, _, _ = bench.read_run(Path(directory) / 'baseline-1.log', skip=0)
            stats = bench.describe(frames)
            self.assertEqual((stats['slow50'], stats['slow80'], stats['hitch30'], stats['pipelines']), (3, 2, 1, 4))

    def test_sums_what_the_main_thread_queued_behind_during_the_race(self):
        with tempfile.TemporaryDirectory() as directory:
            names = ('PARALLEL_REASON reason=0x2824a3398 kind=hook\n'
                     'PARALLEL_REASON reason=0x182acc5ec kind=import name=XNotifyGetNext\n')
            reasons = 'main_blockers_by_reason=37:0x2824a3398:1.500,7:0x0:0.250,16:0x182acc5ec:0.500'
            lines = [names, present(0, 0.016, 0, main_blockers_by_reason='37:0x2824a3398:9.000')]  # the menu
            lines += [present(1 + i, 0.05 + i * 0.04, 1).replace('racing=1', reasons + ' racing=1') for i in range(4)]
            (Path(directory) / 'held-1.log').write_text(''.join(lines), encoding='utf-8')
            table = bench.held_table(directory)
            self.assertIn('| 37 | hook | 0x824a3398 |  | 1.500 |', table)
            self.assertIn('| 16 | import | 0x82acc5ec | XNotifyGetNext | 0.500 |', table)
            self.assertIn('| 7 | 自己的程式 |  |  | 0.250 |', table)
            self.assertLess(table.index('| 37 |'), table.index('| 16 |'))
            # Skipped race frames are left out, like every other number.
            self.assertIn('| 37 | hook | 0x824a3398 |  | 1.500 |', bench.held_table(directory, skip=2))

if __name__ == '__main__':
    unittest.main()
