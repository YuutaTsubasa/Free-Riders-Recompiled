import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import analyse_detectors as analysis

BASE = 0x82000000


def lwz(d, a, offset): return (32 << 26) | (d << 21) | (a << 16) | (offset & 0xFFFF)
def lfs(d, a, offset): return (48 << 26) | (d << 21) | (a << 16) | (offset & 0xFFFF)
def stw(d, a, offset): return (36 << 26) | (d << 21) | (a << 16) | (offset & 0xFFFF)
def lis(d, value): return (15 << 26) | (d << 21) | value
def addi(d, a, value): return (14 << 26) | (d << 21) | (a << 16) | (value & 0xFFFF)
def mr(a, s): return (31 << 26) | (s << 21) | (a << 16) | (s << 11) | (444 << 1)
MTCTR_R10 = (31 << 26) | (10 << 21) | (9 << 16) | (467 << 1)
BCTRL = (19 << 26) | (20 << 21) | (528 << 1) | 1
BLR = (19 << 26) | (20 << 21) | (16 << 1)
FCMPU = (63 << 26) | (6 << 23) | (13 << 16) | (0 << 11)


def bl(from_address, to_address):
    return (18 << 26) | ((to_address - from_address) & 0x03FFFFFC) | 1


def image(words, floats):
    data = bytearray(0x2000)
    for i, word in enumerate(words):
        struct.pack_into('>I', data, i * 4, word)
    for address, value in floats.items():
        struct.pack_into('>f', data, address - BASE, value)
    return analysis.Image.from_bytes(BASE, bytes(data))


class DetectorAnalysisTests(unittest.TestCase):
    def setUp(self):
        # A detector as the title writes them: ask the source for the body,
        # keep it in r31, compare body fields with a constant.
        self.words = [
            lwz(3, 4, 0), lwz(11, 3, 0), lwz(10, 11, 4), MTCTR_R10, BCTRL,
            mr(31, 3),
            lis(11, 0x8200), lfs(0, 11, 0x1000),        # a threshold of 0.35
            lfs(13, 31, 640),                           # body+640
            addi(9, 31, 700), lfs(12, 9, 4),            # body+704 through a pointer
            FCMPU,
            stw(0, 31, 8),                              # a write to body+8
            bl(BASE + 13 * 4, BASE + 0x100),            # calls clobber r3..r12, not r31
            lfs(1, 31, 32),                             # still body+32
            BLR,
        ]
        self.image = image(self.words, {0x82001000: 0.35})

    def test_the_body_record_is_followed_and_constants_are_read(self):
        lines, facts = analysis.disassemble(self.image, BASE, BASE + len(self.words) * 4)
        self.assertEqual(sorted(facts['body']), [8, 32, 640, 704])
        self.assertIn((0x82001000, 'f32', '0.35'), facts['constants'])
        self.assertEqual(facts['calls'], [BASE + 0x100])
        self.assertTrue(facts['writes'][8])
        text = {address: (instruction, note) for address, instruction, note in lines}
        self.assertEqual(text[BASE + 4 * 4], ('bctrl', 'returns the body record in r3'))
        self.assertEqual(text[BASE + 8 * 4], ('lfs f13,640(r31)', 'body+640'))
        self.assertEqual(text[BASE + 7 * 4][1], '[0x82001000] = 0.35')
        self.assertEqual(text[BASE + 11 * 4][0], 'fcmpu cr6,f13,f0')
        self.assertEqual(lines[-1][1], 'blr')

    def test_a_call_forgets_what_the_volatile_registers_held(self):
        words = [lwz(3, 4, 0), lwz(11, 3, 0), lwz(10, 11, 4), MTCTR_R10, BCTRL,
                 bl(BASE + 5 * 4, BASE + 0x100), lfs(1, 3, 12), BLR]
        _, facts = analysis.disassemble(image(words, {}), BASE, BASE + len(words) * 4)
        self.assertEqual(facts['body'], {}, 'r3 after a call is no longer the body record')

    def test_functions_end_where_the_next_begins(self):
        mapping = '{ 0x82000000, sub_82000000 },\n{ 0x82000040, sub_82000040 },\n{ 0x82000100, sub_82000100 },'
        self.assertEqual(analysis.function_ends(mapping), {0x82000000: 0x82000040, 0x82000040: 0x82000100})

    def test_the_report_summarises_each_detector(self):
        ends = {BASE: BASE + len(self.words) * 4, BASE + 0x100: BASE + 0x104}
        self.image.data = bytearray(self.image.data)
        struct.pack_into('>I', self.image.data, 0x100, BLR)
        self.image.data = bytes(self.image.data)
        text = analysis.report(self.image, ends, [(BASE, 'jump'), (0x82999999, 'missing')])
        self.assertIn('| 0x82000000 | jump | +8, +32, +640, +704 | 0.35 | 0x82000100 |', text)
        self.assertIn('(not a function in the mapping)', text)
        self.assertIn('### called: 0x82000100', text)


if __name__ == '__main__':
    unittest.main()
