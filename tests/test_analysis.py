import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
try:
    from analyse_game import find_unique_aligned
except ImportError:
    find_unique_aligned = None


class PatternTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(find_unique_aligned, 'analysis implementation is missing')

    def test_uses_virtual_address_and_ignores_unaligned_decoy(self):
        self.assertEqual(find_unique_aligned(b'\0abcd\0\0\0abcd', b'abcd', 0x82000000), 0x82000008)

    def test_rejects_ambiguous_signature(self):
        with self.assertRaisesRegex(ValueError, 'ambiguous'):
            find_unique_aligned(b'abcdabcd', b'abcd', 0x82000000)

    def test_rejects_missing_signature(self):
        with self.assertRaisesRegex(ValueError, 'missing'):
            find_unique_aligned(b'abc', b'abcd', 0x82000000)

    def test_rejects_empty_signature(self):
        with self.assertRaises(ValueError):
            find_unique_aligned(b'abcd', b'', 0x82000000)


if __name__ == '__main__':
    unittest.main()
