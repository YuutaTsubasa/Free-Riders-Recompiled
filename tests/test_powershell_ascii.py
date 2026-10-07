"""Windows PowerShell 5.1 reads a script without a byte order mark in the system's
code page (Big5 on a Traditional Chinese Windows): any other byte in a .ps1 can
break its parsing there, as Chinese headings once broke benchmark_all.ps1."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PowerShellAsciiTest(unittest.TestCase):
    def test_every_script_is_ascii(self):
        for script in sorted((ROOT / 'scripts').glob('*.ps1')) + sorted(ROOT.glob('*.bat')):
            data = script.read_bytes()
            offenders = [n for n, line in enumerate(data.split(b'\n'), 1) if any(b > 127 for b in line)]
            self.assertEqual(offenders, [], f'{script.name}: lines {offenders[:5]} are not ASCII')


if __name__ == '__main__':
    unittest.main()
