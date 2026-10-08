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


def write_race(directory, name, frame_ms, race_frames=2400, **fields):
    """A log of race_frames race frames of frame_ms each, after a short menu: every run
    of a benchmark has as many race frames, so a faster one is a shorter race."""
    lines, seconds, frame = [], 0.0, 0
    for _ in range(3):
        seconds += 0.016
        lines.append(present(frame, seconds, 0))
        frame += 1
    for _ in range(race_frames):
        seconds += frame_ms / 1000
        lines.append(present(frame, seconds, 1, **fields))
        frame += 1
    lines.append('STOP present-limit @0x0: SFR_PRESENT_LIMIT_AFTER_SAY reached\n')
    (Path(directory) / name).write_text(''.join(lines), encoding='utf-8')


class BottleneckTableTest(unittest.TestCase):
    def test_half_resolution_fps_survives_the_heat_check(self):
        # Three or more baseline rounds run the heat check; it once reused the name of the
        # half-resolution fps, and the table printed the number of rounds over two instead.
        with tempfile.TemporaryDirectory() as directory:
            for repeat in (1, 2, 3, 4):
                write_race(directory, f'baseline-{repeat}.log', 40, race_frames=1500)
                # 30 ms, not 32: 31.25 fps sits on a rounding edge that Python 3.12's sum() falls on the other side of.
                write_race(directory, f'skip-draws-{repeat}.log', 30, race_frames=1500)
                write_race(directory, f'render-50-{repeat}.log', 39, race_frames=1500)
            table, _ = bench.summarise(directory, 0)
            row = next(line for line in table.splitlines() if 'fps, ' in line and 'median;' in line)
            self.assertIn('| 25.0 | 33.3 (+33%) | 25.6 (+3%) |', row)

    def test_version_row_names_the_configs_once(self):
        with tempfile.TemporaryDirectory() as directory:
            write_race(directory, 'baseline-1.log', 20, race_frames=200)
            (Path(directory) / 'info.txt').write_text(
                'commit=abc1234\nbackend=d3d12\nconfigs=baseline repeats=1 race_frames=5400 scenario=solo fixed_step=True\n', encoding='utf-8')
            table, _ = bench.summarise(directory, 0)
            # The settings are the method's (each part of a full run has its own), not the PC's.
            self.assertIn('| Version | commit abc1234 / backend d3d12 |', table)
            self.assertIn('| Settings | baseline, 1 round each |', table)


class AblationTest(unittest.TestCase):
    def test_each_part_in_ms_with_reversed_settings_and_noise(self):
        with tempfile.TemporaryDirectory() as directory:
            for repeat in (1, 2, 3, 4):
                write_race(directory, f'baseline-{repeat}.log', 40, race_frames=1500)
                write_race(directory, f'skip-draws-{repeat}.log', 32, race_frames=1500)
                write_race(directory, f'no-audio-{repeat}.log', 39.8, race_frames=1500)
                write_race(directory, f'serial-{repeat}.log', 50, race_frames=1500)
                write_race(directory, f'main-unpinned-{repeat}.log', 36, race_frames=1500)
                write_race(directory, f'main-pinned-{repeat}.log', 45, race_frames=1500)
            table, _ = bench.summarise(directory, 0)
            parts = table.split('### What each part costs', 1)[1].split('### Details', 1)[0]
            self.assertIn('| skip-draws | nothing drawn | 32.0 | -8.0 ms | 1.25 1.25 1.25 1.25 | drawing (CPU and GPU) made free would save at most: 8.0 ms |', parts)
            self.assertIn('| no-audio |', parts)
            self.assertIn('inside the noise', parts.split('| no-audio |')[1].splitlines()[0])
            self.assertIn('read the other way: what running in parallel saves now: 10.0 ms', parts)
            # In the order of ABLATIONS, whatever order the runs were in.
            self.assertLess(parts.index('| skip-draws |'), parts.index('| no-audio |'))
            self.assertLess(parts.index('| no-audio |'), parts.index('| serial |'))
            # Faster without the pin: the pin costs on this PC.
            self.assertIn('| main-unpinned | main thread not pinned to the first core | 36.0 | -4.0 ms |', parts)
            self.assertIn('turned off it is 4.0 ms faster: this optimization costs on this PC', parts)
            # Slower pinned (the default with fewer than six processors leaves it unpinned).
            self.assertIn('| main-pinned | main thread pinned to the first core | 45.0 | +5.0 ms |', parts)
            self.assertIn('what leaving the main thread unpinned saves on this PC: 5.0 ms', parts)

    def test_no_table_without_baseline_or_ablations(self):
        with tempfile.TemporaryDirectory() as directory:
            for repeat in (1, 2):
                write_race(directory, f'baseline-{repeat}.log', 40, race_frames=1500)
                write_race(directory, f'exe-b-{repeat}.log', 38, race_frames=1500)
            table, _ = bench.summarise(directory, 0)
            self.assertNotIn('What each part costs', table)


def write_frames(directory, name, frame_ms_list, draws=800):
    """A log whose race frames take the given ms each, after a short menu."""
    lines, seconds = [], 0.0
    for frame in range(3):
        seconds += 0.016
        lines.append(present(frame, seconds, 0))
    for frame, ms in enumerate(frame_ms_list, start=3):
        seconds += ms / 1000
        lines.append(present(frame, seconds, 1).replace(' draws=800 ', f' draws={draws + (400 if ms > 40 else 0)} '))
    lines.append('STOP present-limit @0x0: SFR_PRESENT_LIMIT_RACING reached\n')
    (Path(directory) / name).write_text(''.join(lines), encoding='utf-8')


class SlowStretchTest(unittest.TestCase):
    INFO = 'configs=baseline repeats=3 race_frames=3000 scenario=solo fixed_step={} stretch=True capped=False\n'

    def make(self, directory, fixed='True', heavy_runs=(1, 2, 3)):
        for repeat in (1, 2, 3):
            ms = [30.0] * 3000
            if repeat in heavy_runs:
                ms[1200:1260] = [60.0] * 60          # race frames 1200-1259 (the first race frame is 0)
            if repeat == 1:
                ms[2400:2430] = [90.0] * 30          # one run's hitch: not a scene
            write_frames(directory, f'baseline-{repeat}.log', ms)
        (Path(directory) / 'info.txt').write_text(self.INFO.format(fixed), encoding='utf-8')

    def test_a_stretch_slow_in_most_runs_is_listed_by_race_frame(self):
        with tempfile.TemporaryDirectory() as directory:
            self.make(directory)
            table, _ = bench.summarise(directory, 600)
            slow = table.split('### The slowest stretches of the race', 1)[1].split('### Details', 1)[0]
            # The two heavy half seconds are one scene, one row.
            self.assertIn('| 1200–1259 | 20.0–21.0 s | 60.0 / 60.0 |', slow)
            self.assertNotIn('| 1230–', slow)
            self.assertIn('| 3/3 | 1200 |', slow)
            self.assertNotIn('2400–2429', slow)

    def test_scattered_slow_frames_name_no_scene(self):
        with tempfile.TemporaryDirectory() as directory:
            self.make(directory, heavy_runs=(1,))
            table, _ = bench.summarise(directory, 600)
            self.assertIn('No stretch is heavy in most runs', table)

    def test_only_with_a_fixed_step(self):
        with tempfile.TemporaryDirectory() as directory:
            self.make(directory, fixed='False')
            table, _ = bench.summarise(directory, 600)
            self.assertNotIn('The slowest stretches of the race', table)


class FullReportTest(unittest.TestCase):
    def test_report_gives_the_pc_once_then_race_then_parts(self):
        with tempfile.TemporaryDirectory() as directory:
            for name, ms in (('parts', 30), ('race', 36)):
                (Path(directory) / name).mkdir()
                write_race(Path(directory) / name, 'baseline-1.log', ms, race_frames=200)
                (Path(directory) / name / 'info.txt').write_text(
                    f'cpu=Test CPU\nconfigs=baseline repeats=1 scenario={"solo" if name == "parts" else "race"} '
                    f'fixed_step={name == "parts"}\n', encoding='utf-8')
                table, _ = bench.summarise(Path(directory) / name, 0)
                (Path(directory) / name / 'summary.md').write_text(table, encoding='utf-8')
            report = bench.combine_report(directory, 95)
            self.assertTrue(report.startswith('## Performance report (run_benchmark.bat full, 95 minutes)'))
            self.assertEqual(report.count('### Hardware and drivers'), 1)
            self.assertEqual(report.count('### How it was measured'), 2)
            self.assertLess(report.index('## The frame rate a player gets'), report.index('## What each part costs'))
            self.assertLess(report.index('solo (Time Attack'), len(report))
            self.assertEqual((Path(directory) / 'report.md').read_text(encoding='utf-8'), report)

    def test_a_part_that_did_not_finish_is_said(self):
        with tempfile.TemporaryDirectory() as directory:
            (Path(directory) / 'parts').mkdir()
            write_race(Path(directory) / 'parts', 'baseline-1.log', 30, race_frames=200)
            table, _ = bench.summarise(Path(directory) / 'parts', 0)
            (Path(directory) / 'parts' / 'summary.md').write_text(table, encoding='utf-8')
            report = bench.combine_report(directory)
            self.assertIn('(This part did not finish.)', report.split('## What each part costs')[0])


class StallTest(unittest.TestCase):
    def test_runs_that_stood_still_are_named(self):
        with tempfile.TemporaryDirectory() as directory:
            ms = [20.0] * 3000
            ms[1500] = 2100.0
            ms[2000] = 6400.0
            write_frames(directory, 'baseline-1.log', ms)
            write_frames(directory, 'baseline-2.log', [20.0] * 3000)
            table, _ = bench.summarise(directory, 600)
            self.assertIn('The race stopped for 1 second or more', table)
            self.assertIn('baseline-1: 2 times, 8.5 s in all (longest 6.4 s)', table)
            self.assertNotIn('baseline-2:', table)

    def test_no_note_without_stalls(self):
        with tempfile.TemporaryDirectory() as directory:
            write_frames(directory, 'baseline-1.log', [20.0] * 1000 + [900.0] + [20.0] * 1000)
            table, _ = bench.summarise(directory, 600)
            self.assertNotIn('The race stopped', table)


class MethodTest(unittest.TestCase):
    def test_fixed_step_race_time_is_the_frame_number(self):
        # With one original frame per present, a slow and a fast run cover the same race time
        # in as many frames, so the window is the same frames in both.
        with tempfile.TemporaryDirectory() as directory:
            write_race(directory, 'slow-1.log', 40, race_frames=3000)
            write_race(directory, 'fast-1.log', 10, race_frames=3000)
            for name in ('slow-1.log', 'fast-1.log'):
                timeline = []
                bench.read_run(Path(directory) / name, 0, timeline, fixed_step=True)
                self.assertAlmostEqual(timeline[-1][0], 2999 / 60)
                self.assertEqual(sum(1 for t, _ in timeline if 20 <= t < 40), 1200)
            timeline = []
            bench.read_run(Path(directory) / 'fast-1.log', 0, timeline)
            self.assertAlmostEqual(timeline[-1][0], 29.99, places=2)

    def test_method_table_names_scenario_and_clock(self):
        with tempfile.TemporaryDirectory() as directory:
            for repeat in (1, 2):
                write_race(directory, f'baseline-{repeat}.log', 20, race_frames=6000)
            (Path(directory) / 'info.txt').write_text(
                'configs=baseline repeats=2 present_limit=0 after_say= race_frames=5400 scenario=solo fixed_step=True stretch=True capped=False\n'
                'race_frames_from_warmup=2430 warmup_fps=27\n', encoding='utf-8')
            table, _ = bench.summarise(directory, 0)
            self.assertIn('### How it was measured', table)
            self.assertIn('solo (Time Attack, no rivals)', table)
            self.assertIn('each frame advances the race 1/60 s', table)
            # The counted runs ended where the warm-up said, not at the configured 5400.
            self.assertIn('| Each run ends | at race frame 2430 (race frames only; the warm-up ran 27 fps', table)
            self.assertNotIn('race frame 5400', table)
            self.assertIn('race frames 1200–4500', table)
            # What decides is open; every run and the whole-run comparison are folded after it.
            open_part, details = table.split('### Details', 1)
            self.assertIn('### The same stretch of race', open_part)
            self.assertNotIn('<details>', open_part)
            self.assertIn('<summary>Every run (2 runs)</summary>', details)
            self.assertIn('<summary>The whole run (every race frame of each run)</summary>', details)
            self.assertEqual(details.count('<details>'), details.count('</details>'))

    def test_old_info_says_what_it_does_not_know(self):
        lines = '\n'.join(bench.method_lines({'after_say': '4200', 'capped': 'False'}))
        self.assertIn('not recorded (an older benchmark.ps1: a race)', lines)
        self.assertIn('the loading screen counts too', lines)
        self.assertIn('the frame rate a player gets', lines)


class BenchmarkSummaryTest(unittest.TestCase):
    def test_failed_runs_and_warmup_do_not_define_expected_length(self):
        rows = [('baseline', 1, {'frames': 100}, 'present-limit'),
                ('warmup', 1, {'frames': 60}, 'present-limit'),
                ('crashed', 1, {'frames': 60}, 'native-draw'),
                ('crashed', 2, {'frames': 60}, 'native-draw')]
        good, _ = bench.valid_runs(rows)
        self.assertEqual([row[0] for row in good], ['baseline'])

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
            # The warm-up is listed with the runs (folded at the end) but not compared.
            comparison, runs = table.split('### Details', 1)
            self.assertIn('<details>\n<summary>Every run (4 runs)</summary>\n\n| Setting |', runs)
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
            self.assertIn('faster', fast_row)
            self.assertIn('5/5', fast_row)
            self.assertIn('no verdict', noisy_row)

    def test_too_few_rounds_prove_nothing(self):
        with tempfile.TemporaryDirectory() as directory:
            for repeat in (1, 2):
                write_log(directory, f'baseline-{repeat}.log', 40)
                write_log(directory, f'fast-{repeat}.log', 20)
            table, _ = bench.summarise(directory, skip=0)
            self.assertIn('too few rounds', table)

    def test_a_run_that_did_not_reach_the_limit_or_raced_less_is_left_out_of_the_comparison(self):
        with tempfile.TemporaryDirectory() as directory:
            for repeat in range(1, 5):
                write_log(directory, f'baseline-{repeat}.log', 40, race=100)
            write_log(directory, 'crashed-1.log', 40, race=100, stop='STOP native-draw @0x7e: boom')
            write_log(directory, 'short-1.log', 40, race=60)
            table, _ = bench.summarise(directory, skip=0)
            self.assertIn('Left out of the comparison', table)
            self.assertIn('crashed-1', table.split('Left out of the comparison')[1])
            self.assertIn('short-1', table.split('Left out of the comparison')[1])
            comparison = table.split('### Details', 1)[0]
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
            self.assertIn('| 7 | its own code |  |  | 0.250 |', table)
            self.assertLess(table.index('| 37 |'), table.index('| 16 |'))
            # Skipped race frames are left out, like every other number.
            self.assertIn('| 37 | hook | 0x824a3398 |  | 1.500 |', bench.held_table(directory, skip=2))
    def test_race_window_counts_the_same_seconds_of_every_run(self):
        with tempfile.TemporaryDirectory() as directory:
            timeline = []
            write_race(directory, 'baseline-1.log', 40)
            bench.read_run(Path(directory) / 'baseline-1.log', 0, timeline)
            self.assertAlmostEqual(bench.window_fps(timeline, 20, 75), 25.0, places=3)
            self.assertIsNone(bench.window_fps(timeline, 200, 300))
            for n in (1, 2, 3, 4):
                write_race(directory, f'baseline-{n}.log', 40)
                write_race(directory, f'all-{n}.log', 32)
            table, _ = bench.summarise(directory, skip=0)
            self.assertIn('race seconds 20–75', table)
            self.assertIn('| all | 1.25 1.25 1.25 1.25 | 1.25 | 4/4 | faster |', table)

    def test_breakdown_takes_each_overlapping_part_once(self):
        stats = {'fps': 40.0, 'draw_ms': 3.0, 'gpu_wait_ms': 4.0, 'present_ms': 5.0, 'main_blocked_ms': 9.0,
                 'main_queued_ms': 1.0, 'pacing_ms': 2.0}
        frame, parts = bench.frame_parts(stats)
        self.assertEqual(frame, 25.0)
        self.assertEqual(parts['present'], 1.0)   # 5 of present, 4 of them waiting for the GPU
        self.assertEqual(parts['wait'], 3.0)      # 9 of waits, less the GPU's 4 and the cap's 2
        self.assertAlmostEqual(sum(parts.values()), 25.0)
        self.assertEqual(parts['running'], 25.0 - 3 - 4 - 1 - 3 - 1 - 2)

    def test_breakdown_says_when_the_build_does_not_record_waits(self):
        with tempfile.TemporaryDirectory() as directory:
            write_race(directory, 'baseline-1.log', 40, draw_ms=5, present_ms=2, gpu_wait_ms=1)
            table, _ = bench.summarise(directory, skip=0)
            self.assertIn('Where the main thread\'s frame goes', table)
            self.assertIn('did not record the main thread\'s waits', table)
            write_race(directory, 'baseline-1.log', 40, main_blocked_ms=6)
            table, _ = bench.summarise(directory, skip=0)
            self.assertNotIn('did not record the main thread\'s waits', table)

    def test_bottleneck_from_skip_draws_and_half_resolution(self):
        cases = ((40, 37, 39, 'the game\'s own CPU time'), (40, 25, 39, 'drawing path'), (40, 25, 30, 'GPU'))
        for base, skip, half, verdict in cases:
            with tempfile.TemporaryDirectory() as directory:
                for n in (1, 2):
                    write_race(directory, f'baseline-{n}.log', base)
                    write_race(directory, f'skip-draws-{n}.log', skip)
                    write_race(directory, f'render-50-{n}.log', half)
                table, _ = bench.summarise(directory, skip=0)
                self.assertIn('What limits this PC', table)
                self.assertIn(f'Verdict: **{verdict}**', table)
                self.assertIn('60 fps needs 16.7 ms: 23.3 ms a frame still to save (140% faster)', table)

    def test_no_bottleneck_section_without_the_report_settings(self):
        with tempfile.TemporaryDirectory() as directory:
            write_race(directory, 'baseline-1.log', 40)
            write_race(directory, 'skip-draws-1.log', 30)
            table, _ = bench.summarise(directory, skip=0)
            self.assertNotIn('What limits this PC', table)
    def test_a_fast_pc_short_race_still_gets_a_common_stretch(self):
        with tempfile.TemporaryDirectory() as directory:
            for n in (1, 2):   # 4200 race presents at 125 fps: a 34-second race
                write_race(directory, f'baseline-{n}.log', 8, race_frames=4200)
                write_race(directory, f'skip-draws-{n}.log', 7, race_frames=4200)
                write_race(directory, f'render-50-{n}.log', 8, race_frames=4200)
            table, _ = bench.summarise(directory, skip=0)
            self.assertIn('### The same stretch of race (race seconds 10–29)', table)
            self.assertIn('A frame takes 8.0 ms now, past 120 fps.', table)

    def test_the_gap_is_to_120_fps_once_past_60(self):
        with tempfile.TemporaryDirectory() as directory:
            for n in (1, 2):
                write_race(directory, f'baseline-{n}.log', 10, race_frames=6000)
                write_race(directory, f'skip-draws-{n}.log', 9, race_frames=6000)
                write_race(directory, f'render-50-{n}.log', 10, race_frames=6000)
            table, _ = bench.summarise(directory, skip=0)
            self.assertIn('120 fps needs 8.3 ms: 1.7 ms a frame still to save (20% faster)', table)
            self.assertIn('(Already past 60 fps.)', table)

    def test_a_capped_run_names_no_bottleneck(self):
        with tempfile.TemporaryDirectory() as directory:
            for n in (1, 2):
                write_race(directory, f'baseline-{n}.log', 16.7)
                write_race(directory, f'skip-draws-{n}.log', 16.7)
                write_race(directory, f'render-50-{n}.log', 16.7)
            (Path(directory) / 'info.txt').write_text(
                'configs=baseline,skip-draws,render-50 repeats=2 present_limit=0 capped=True\n', encoding='utf-8')
            table, _ = bench.summarise(directory, skip=0)
            self.assertIn('This ran with a frame limit', table)
            self.assertNotIn('Verdict:', table)

    def test_a_gpu_wait_beyond_the_present_comes_out_of_the_draw(self):
        # A ring flush waits for the GPU inside a draw: 6 ms of GPU wait, 4 of them in the present.
        stats = {'fps': 50.0, 'draw_ms': 5.0, 'gpu_wait_ms': 6.0, 'present_ms': 4.0, 'main_blocked_ms': 6.0,
                 'main_queued_ms': 0.0, 'pacing_ms': 0.0}
        frame, parts = bench.frame_parts(stats)
        self.assertEqual((parts['present'], parts['draw_ms'], parts['gpu_wait_ms'], parts['wait']), (0.0, 3.0, 6.0, 0.0))
        self.assertAlmostEqual(sum(parts.values()), 20.0)

    def test_the_verdict_pairs_rounds_and_doubts_a_gap_within_the_spread(self):
        with tempfile.TemporaryDirectory() as directory:
            # The PC is slower in rounds 2 and 4 (26% baseline spread), but half resolution
            # is 11% faster in every round: GPU, said with a warning.
            for n, base in ((1, 40), (2, 52), (3, 40), (4, 52)):
                write_race(directory, f'baseline-{n}.log', base)
                write_race(directory, f'skip-draws-{n}.log', base * 0.98)
                write_race(directory, f'render-50-{n}.log', base * 0.9)
            table, _ = bench.summarise(directory, skip=0)
            self.assertIn('Verdict: **GPU**: half resolution is 11% faster', table)
            self.assertIn('**The verdict is unreliable**', table)
            self.assertIn('the baseline\'s own spread (26%)', table)
            self.assertNotIn('slower than its earlier ones', table)

    def test_the_verdict_notes_a_baseline_that_slows_round_by_round(self):
        with tempfile.TemporaryDirectory() as directory:
            for n, base in ((1, 40), (2, 42), (3, 44), (4, 46)):   # 25 fps down to 21.7: heat
                write_race(directory, f'baseline-{n}.log', base)
                write_race(directory, f'skip-draws-{n}.log', base * 0.98)
                write_race(directory, f'render-50-{n}.log', base * 0.98)
            table, _ = bench.summarise(directory, skip=0)
            self.assertIn('The baseline\'s later rounds are slower than its earlier ones (mean 24.4 → 22.2 fps)', table)
            self.assertIn('Verdict: **the game\'s own CPU time**', table)

    def test_the_hardware_section_comes_first_from_info_and_the_games_own_log(self):
        with tempfile.TemporaryDirectory() as directory:
            write_race(directory, 'baseline-1.log', 40)
            log = Path(directory) / 'baseline-1.log'
            log.write_text('NATIVE_GRAPHICS_D3D12_PROBE adapter=0 name="Radeon RX 480" command_list7=0x80004002 (no D3D12 Agility SDK runtime in D3D12\\?)\n'
                           'NATIVE_GRAPHICS_FALLBACK from=D3D12 to=Vulkan reason=no-usable-d3d12-adapter\n'
                           'NATIVE_GRAPHICS backend=Vulkan adapter=Radeon RX 480 vendor=0x1002 type=discrete vram_mb=8192 driver=2.0.302\n'
                           'NATIVE_RENDER_SCALE percent=100 logical=1280x720 physical=1280x720\n'
                           + log.read_text(encoding='utf-8'), encoding='utf-8')
            (Path(directory) / 'info.txt').write_text(
                'commit=5caf920\nmodel=Dell Inc. OptiPlex 7010\ncpu=Intel(R) Core(TM) i5-3470 CPU @ 3.20GHz\n'
                'gpu=Radeon RX 480\npower=no battery (desktop) plan=High performance idle_cpu_percent=1\n'
                'configs=baseline repeats=1 present_limit=0 after_say=4200 reference_fps=100 stretch=True capped=False\n'
                'cpu_cores=4 cores, 4 threads, max 3200 MHz\ncpu_features=avx=yes avx2=no avx512f=no (the game needs AVX)\n'
                'memory=16 GB, 2 modules at 1600 MT/s\ngpu0=Radeon RX 480 driver=31.0.21912.14 (2023-04-05) vram_mb=8192 display=1920x1080@60\n'
                'vbs=off memory_integrity=on\nos_build=Windows 10 Pro 22H2 build 19045.4046\n', encoding='utf-8')
            table, _ = bench.summarise(directory, skip=0)
            self.assertTrue(table.startswith('### Hardware and drivers'))
            self.assertIn('| CPU | Intel(R) Core(TM) i5-3470 CPU @ 3.20GHz / 4 cores, 4 threads, max 3200 MHz |', table)
            self.assertIn('| GPU the game used (its own log) | backend=Vulkan adapter=Radeon RX 480 vendor=0x1002 type=discrete vram_mb=8192 driver=2.0.302 |', table)
            self.assertIn('| Operating system | Windows 10 Pro 22H2 build 19045.4046 |', table)
            self.assertIn('**D3D12 could not be used; the game fell back to Vulkan**', table)
            self.assertIn('no ID3D12GraphicsCommandList7', table)
            self.assertIn('Memory integrity (VBS) is on', table)
            # The table that follows is still the first thing after the section.
            self.assertLess(table.index('### Hardware and drivers'), table.index('| Setting | Run |'))

    def test_read_machine_prefers_a_baseline_log_and_reads_only_its_head(self):
        with tempfile.TemporaryDirectory() as directory:
            (Path(directory) / 'all-1.log').write_text('NATIVE_GRAPHICS backend=D3D12 adapter=B\n', encoding='utf-8')
            (Path(directory) / 'baseline-1.log').write_text('x\n' * 10 + 'NATIVE_GRAPHICS backend=D3D12 adapter=A\n', encoding='utf-8')
            self.assertEqual(bench.read_machine(directory)['graphics'], 'backend=D3D12 adapter=A')
            self.assertEqual(bench.read_machine(directory, head=5)['graphics'], 'backend=D3D12 adapter=B')
            self.assertEqual(bench.read_machine(Path(directory) / 'none'), {})

if __name__ == '__main__':
    unittest.main()
