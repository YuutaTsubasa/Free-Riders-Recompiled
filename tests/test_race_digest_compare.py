from pathlib import Path
import contextlib
import io
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import race_digest_compare as compare


def log(directory, name, racer_words, address):
    """A game.log with two samples of two racers; word 0 is an address."""
    lines = []
    for step, words in racer_words:
        lines.append(f'RACE_STATE step={step} racers=2 clock=1,2,3,')
        for racer in range(2):
            values = [address + 16 * racer] + words
            lines.append(f'RACE_RACER step={step} racer={racer} words=' + ','.join(f'{v:x}' for v in values) + ',')
    path = Path(directory) / name / 'game.log'
    path.parent.mkdir()
    path.write_text('\n'.join(lines) + '\n')
    return str(path)


def run(argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out), self_exit() as code:
        compare.main(argv)
    return code[0], out.getvalue()


@contextlib.contextmanager
def self_exit():
    code = [0]
    try:
        yield code
    except SystemExit as error:
        code[0] = error.code


class RaceDigestCompareTest(unittest.TestCase):
    def test_a_candidate_that_runs_a_baseline_race_passes(self):
        with tempfile.TemporaryDirectory() as directory:
            a = log(directory, 'a', [(10, [1, 2]), (20, [3, 4]), (30, [5, 6])], 0xE0001000)
            b = log(directory, 'b', [(10, [1, 2]), (20, [3, 4]), (30, [5, 9])], 0xE0002000)
            same = log(directory, 'same', [(10, [1, 2]), (20, [3, 4]), (30, [5, 6])], 0xE0003000)
            other = log(directory, 'other', [(10, [1, 2]), (20, [7, 4]), (30, [5, 6])], 0xE0004000)
            code, text = run(['--baseline', a, b, '--candidate', same])
            self.assertEqual(code, 0, text)
            self.assertIn('agrees to the end', text)
            code, text = run(['--baseline', a, b, '--candidate', other])
            self.assertEqual(code, 1, text)
            self.assertIn('FAIL', text)


if __name__ == '__main__':
    unittest.main()
