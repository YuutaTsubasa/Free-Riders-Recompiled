"""The launcher's Test options (src/test_options.h) stay in step with the game and Android."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
TABLE = ROOT / 'src' / 'test_options.h'
ARCHIVE = ROOT / 'android/app/src/main/java/com/freeriders/recompiled/DiagnosticsArchive.java'


def table_variables():
    return re.findall(r'\{"(SFR_[A-Z0-9_]+)", TestGroup::', TABLE.read_text(encoding='utf-8'))


class TestOptionsTests(unittest.TestCase):
    def test_table_lists_options(self):
        variables = table_variables()
        self.assertGreater(len(variables), 5)
        self.assertEqual(len(variables), len(set(variables)), 'each variable is listed once')

    def test_game_reads_every_option(self):
        # A renamed or removed switch would leave a choice on the page that does nothing.
        sources = ''.join(path.read_text(encoding='utf-8', errors='replace')
                          for path in (ROOT / 'src').rglob('*')
                          if path.suffix in ('.cpp', '.h', '.mm') and path.name != 'test_options.h'
                          and not path.name.startswith('launcher_'))
        for variable in table_variables():
            self.assertIn('"%s"' % variable, sources, variable + ' is read by the game')

    def test_android_diagnostics_keep_every_option(self):
        archive = ARCHIVE.read_text(encoding='utf-8')
        allowlist = archive[archive.index('SETTINGS = new HashSet'):archive.index('));', archive.index('SETTINGS = new HashSet'))]
        for variable in table_variables():
            self.assertIn('"%s"' % variable, allowlist, variable + ' is kept in the diagnostic ZIP')


if __name__ == '__main__':
    unittest.main()
