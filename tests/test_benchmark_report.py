from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import benchmark_report as report
from test_benchmark_summary import write_log


class BenchmarkReportTest(unittest.TestCase):
    def test_only_runs_that_count_get_a_frame_file_and_the_rest_are_named(self):
        with tempfile.TemporaryDirectory() as root:
            folder = Path(root) / 'run'
            folder.mkdir()
            (folder / 'info.txt').write_text('cpu=Some CPU\ngpu=Some GPU\npower=on mains plan=Turbo\n', encoding='utf-8')
            for repeat in range(1, 5):
                write_log(folder, f'baseline-{repeat}.log', 40, race=100)
            write_log(folder, 'crashed-1.log', 40, race=100, stop='STOP native-draw @0x7e: boom')
            write_log(folder, 'never-1.log', 40, race=0)
            notes = Path(root) / 'notes.md'
            notes.write_text('## 觀察\n\n一則備註。\n', encoding='utf-8')
            destination = Path(root) / 'out'
            kept, left_out = report.build(folder, destination, 'Some Laptop', notes, skip=0)
            self.assertEqual((kept, left_out), (4, 2))
            text = (destination / 'report.md').read_text(encoding='utf-8')
            for expected in ('Some Laptop', 'Some CPU', 'crashed-1', 'never-1', '一則備註'):
                self.assertIn(expected, text)
            self.assertEqual(sorted(p.name for p in (destination / 'frames').iterdir()),
                             [f'baseline-{n}.csv' for n in range(1, 5)])
            self.assertEqual(len((destination / 'runs.csv').read_text(encoding='utf-8').splitlines()), 5)
            self.assertEqual(len((destination / 'frames' / 'baseline-1.csv').read_text(encoding='utf-8').splitlines()), 101)

    def test_the_runs_left_out_can_be_kept_out_of_the_report(self):
        with tempfile.TemporaryDirectory() as root:
            folder = Path(root) / 'run'
            folder.mkdir()
            for repeat in range(1, 5):
                write_log(folder, f'baseline-{repeat}.log', 40, race=100)
            write_log(folder, 'never-1.log', 40, race=0)
            destination = Path(root) / 'out'
            report.build(folder, destination, 'Some Laptop', None, skip=0, list_left_out=False)
            text = (destination / 'report.md').read_text(encoding='utf-8')
            self.assertNotIn('never-1', text)
            self.assertNotIn('沒有納入', text)
            self.assertIn('baseline', text)


if __name__ == '__main__':
    unittest.main()
