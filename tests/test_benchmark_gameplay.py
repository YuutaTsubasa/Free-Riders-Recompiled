import sys
import tempfile
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from benchmark_gameplay import inspect_gameplay, inspect_runs


def fixture(loading=2, config='1:2:1:0:3:60:0'):
    lines = []
    for frame in range(loading+306):
        tick = max(0, frame-loading+1)
        cd, race = min(tick, 300), max(0, tick-300)
        values = dict(frame=frame, draw_timers=0, presented=1,
            frame_ms=500 if tick == 0 else 40 if tick <= 300 else 20,
            racing=1, gameplay_schema=1, game_contract='solo_fixed_v1', game_run=1,
            game_state='prestart' if tick == 0 else 'advancing',
            game_stage='none' if tick == 0 else 'countdown' if race == 0 else 'racing',
            game_start_observed=int(tick > 0), game_phase=9 if tick == 0 else 10 if tick < 300 else 11,
            game_tick=tick, game_countdown_tick=cd, game_race_tick=race,
            game_countdown_ms=f'{max(0, 5000-cd*16.666667938):.9f}',
            game_elapsed_ms=f'{race*16.666667938:.9f}', game_config=config)
        lines.append('NATIVE_PRESENT '+' '.join(f'{k}={v}' for k,v in values.items()))
    return lines+['STOP gameplay-limit @0x132: SFR_GAMEPLAY_TICK_LIMIT reached']


class GameplayReportTest(unittest.TestCase):
    def inspect(self, lines):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'run.log'
            path.write_text('\n'.join(lines)+'\n', encoding='utf-8')
            return inspect_gameplay(path, start=2, end=5, limit=306)

    def test_countdown_is_included_and_separate_from_loading(self):
        a, b = self.inspect(fixture(2)), self.inspect(fixture(99))
        self.assertEqual(a['countdown']['frames'], 300)
        self.assertEqual(a['countdown']['fps'], 25)
        self.assertEqual(a['countdown']['over_33_33'], 300)
        self.assertEqual(a['racing']['frames'], 6)
        self.assertEqual(a['race_window']['frames'], 3)
        self.assertEqual(a['race_window']['fps'], 50)
        self.assertEqual(a['combined']['frames'], 306)
        self.assertAlmostEqual(a['combined']['mean_ms'], (300*40+6*20)/306)
        self.assertEqual(a['combined'], b['combined'])
        self.assertNotEqual(a['first_present'], b['first_present'])

    def test_last_countdown_frame_is_phase11_and_retained(self):
        source = fixture()
        source[301] = source[301].replace('frame_ms=40', 'frame_ms=200')
        result = self.inspect(source)
        self.assertEqual(result['countdown']['max_ms'], 200)
        self.assertEqual(result['racing']['max_ms'], 20)
        source[301] = source[301].replace('game_stage=countdown', 'game_stage=racing')
        with self.assertRaises(ValueError):
            self.inspect(source)

    def test_legacy_schema_and_stop_cannot_pass(self):
        for source in ([s.replace('gameplay_schema=1', '') for s in fixture()],
                       fixture()[:-1]+['STOP present-limit @0: old counter']):
            with self.assertRaises(ValueError):
                self.inspect(source)

    def test_countdown_final_clamp_and_supported_config_are_required(self):
        source = fixture()
        for frame in range(2,302):
            cd = frame-1
            old = source[frame].split('game_countdown_ms=')[1].split()[0]
            source[frame] = source[frame].replace('game_countdown_ms='+old,
                'game_countdown_ms='+f'{5000-cd*(16.666667938-.001):.9f}')
        with self.assertRaisesRegex(ValueError,'countdown'):
            self.inspect(source)
        with self.assertRaisesRegex(ValueError,'configuration'):
            self.inspect(fixture(config='1:2:1:0:3:60:4'))

    def test_missing_duplicate_tick_present_or_countdown_frame_fails(self):
        source = fixture()
        for changed in (source[:5]+source[6:],
                        source[:5]+[source[5].replace('game_tick=4', 'game_tick=3')]+source[6:],
                        source[:5]+[source[5].replace('frame=5 ', 'frame=4 ')]+source[6:]):
            with self.assertRaises(ValueError):
                self.inspect(changed)

    def test_pause_invalid_exit_contract_epoch_config_and_start_fail(self):
        for old,new in (('game_state=advancing','game_state=paused'),
                        ('game_state=advancing','game_state=invalid'),
                        ('game_state=advancing','game_state=outside'),
                        ('solo_fixed_v1','guessed_mode'), ('game_run=1','game_run=2'),
                        ('game_config=1:2:1:0:3:60:0','game_config=2:2:1:0:3:60:0'),
                        ('game_start_observed=1','game_start_observed=0')):
            source = fixture()
            source[5] = source[5].replace(old,new)
            with self.assertRaises(ValueError):
                self.inspect(source)

    def test_bad_time_duration_diagnostics_and_rendering_fail(self):
        for old,new in (('frame_ms=40','frame_ms=-1'), ('draw_timers=0','draw_timers=1'),
                        ('presented=1','presented=0'), ('game_elapsed_ms=0.000000000','game_elapsed_ms=nan')):
            source = fixture()
            source[5] = source[5].replace(old,new)
            with self.assertRaises(ValueError):
                self.inspect(source)
        source = fixture()
        source[5] += ' race_probe=1'
        with self.assertRaises(ValueError):
            self.inspect(source)

    def test_short_stop_extra_present_and_fault_fail(self):
        for source in (fixture()[:-2]+[fixture()[-1]], fixture()+[fixture()[5]],
                       fixture()[:-1]+['HANG_REPORT test',fixture()[-1]],
                       fixture()[:-1]+['STOP managed-buffers-mismatch @0 test',fixture()[-1]]):
            with self.assertRaises(ValueError):
                self.inspect(source)

    def test_prefixed_present_is_preserved(self):
        source = fixture()
        source[5] = 'RESULT NtCreateEvent frame=999 type=0'+source[5]
        self.assertEqual(self.inspect(source)['combined']['frames'],306)

    def test_configuration_mismatch_is_not_compared(self):
        with tempfile.TemporaryDirectory() as directory:
            a,b = Path(directory)/'a.log',Path(directory)/'b.log'
            a.write_text('\n'.join(fixture()))
            b.write_text('\n'.join(fixture(config='2:2:1:0:3:60:0')))
            with self.assertRaisesRegex(ValueError,'different race configurations'):
                inspect_runs([a,b],start=2,end=5,limit=306)


if __name__ == '__main__':
    unittest.main()
