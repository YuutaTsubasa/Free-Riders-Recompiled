import ntpath
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import tomllib

try:
    from prepare_recomp import relative_source_path, toml_value
except ImportError:
    relative_source_path = toml_value = None


class RecompPathTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(relative_source_path, 'preflight path validation is missing')

    def test_cross_drive_source_has_actionable_preflight_error(self):
        with self.assertRaisesRegex(ValueError, 'same drive'):
            relative_source_path(r'D:\game\default.xex', r'C:\project\out', ntpath)

    def test_same_drive_source_is_relative_to_config_directory(self):
        self.assertEqual(relative_source_path(r'C:\game\default.xex', r'C:\project\out', ntpath),
                         r'..\..\game\default.xex')


class RecompConfigTests(unittest.TestCase):
    def test_project_configuration_round_trips_for_the_recompiler(self):
        config = tomllib.loads((Path(__file__).resolve().parents[1] / 'config/freeriders.toml')
                               .read_text())['main']
        text = '[main]\n' + ''.join(f'{key} = {toml_value(value)}\n' for key, value in config.items())
        self.assertEqual(tomllib.loads(text)['main'], config)

    def test_invalid_instruction_tables_are_inline_and_hexadecimal(self):
        self.assertEqual(toml_value([{'data': 0x82A5C368, 'size': 8}]),
                         '[{ data = 0x82a5c368, size = 0x8 }]')

    def test_unknown_value_types_are_rejected(self):
        with self.assertRaises(TypeError):
            toml_value(1.5)
