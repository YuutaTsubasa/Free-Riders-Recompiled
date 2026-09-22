import os
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import analyse_switches as sw
from generate_diagnostic import COUNTRY_CTR, FORMAT_CTR, rewrite_jump_tables

B = sw.IMAGE_BASE


# Minimal PowerPC encoders for the instructions these fixtures use.
def d_form(op, d, a, imm): return (op << 26) | (d << 21) | (a << 16) | (imm & 0xFFFF)
def lis(d, imm): return d_form(15, d, 0, imm)
def addi(d, a, imm): return d_form(14, d, a, imm)
def ori(a, s, imm): return d_form(24, s, a, imm)
def lwz(d, a, imm): return d_form(32, d, a, imm)
def cmplwi(field, a, imm): return d_form(10, field << 2, a, imm)
def rlwinm(a, s, sh, mb, me): return (21 << 26) | (s << 21) | (a << 16) | (sh << 11) | (mb << 6) | (me << 1)
def x31(d, a, b, xo): return (31 << 26) | (d << 21) | (a << 16) | (b << 11) | (xo << 1)
def lwzx(d, a, b): return x31(d, a, b, 23)
def lbzx(d, a, b): return x31(d, a, b, 87)
def add(d, a, b): return x31(d, a, b, 266)
def mr(a, s): return x31(s, a, s, 444)
def mtctr(s): return 0x7C0903A6 | (s << 21)
def bgt(field, source, target): return (16 << 26) | (12 << 21) | ((field * 4 + 1) << 16) | ((target - source) & 0xFFFC)
def bgtlr(field): return 0x4C000020 | (12 << 21) | ((field * 4 + 1) << 16)
def b(source, target): return (18 << 26) | ((target - source) & 0x03FFFFFC)
def bl(source, target): return b(source, target) | 1
BCTR, BLR = sw.BCTR, sw.BLR


class Fixture:
    def __init__(self):
        self.data = bytearray(sw.TEXT_BASE + sw.TEXT_SIZE - B)
        self.pdata_index = 0

    def put(self, address, *words):
        for offset, word in enumerate(words):
            struct.pack_into('>I', self.data, address - B + 4 * offset, word & 0xFFFFFFFF)

    def put_bytes(self, address, values):
        self.data[address - B:address - B + len(values)] = bytes(values)

    def function(self, start, size):
        struct.pack_into('>II', self.data, sw.PDATA_BASE - B + 8 * self.pdata_index,
                         start, (size // 4) << 8)
        self.pdata_index += 1

    def analyse(self):
        tables, rejected, _ = sw.analyse(bytes(self.data))
        return {t['base']: t for t in tables}, {r['bctr']: r['reason'] for r in rejected}


def hi_lo(address):
    low = address & 0xFFFF
    return (address >> 16) + (1 if low & 0x8000 else 0), low


F = 0x82300000


def absolute_switch(fixture, index_register=10, limit=2, prefix=()):
    """cmplwi/bgt then this game's lis, rlwinm, addi, lwzx order, table inline after bctr."""
    table = F + 4 * (len(prefix) + 8)
    high, low = hi_lo(table)
    code = [*prefix, cmplwi(6, index_register, limit), 0, lis(12, high),
            rlwinm(0, index_register, 2, 0, 29), addi(12, 12, low), lwzx(0, 12, 0), mtctr(0), BCTR]
    code[len(prefix) + 1] = bgt(6, F + 4 * (len(prefix) + 1), F + 0x100)
    fixture.put(F, *code)
    cases = [F + 0x80 + 8 * i for i in range(limit + 1)]
    fixture.put(table, *cases)
    for case in cases:
        fixture.put(case, BLR)
    fixture.put(F + 0x100, BLR)
    return F + 4 * (len(prefix) + 7), cases


class SwitchAnalysisTests(unittest.TestCase):
    def test_reordered_absolute_table_labels_come_from_the_image(self):
        fixture = Fixture()
        fixture.function(F, 0x200)
        bctr, cases = absolute_switch(fixture)
        tables, _ = fixture.analyse()
        self.assertEqual(tables[bctr]['labels'], cases)
        self.assertEqual((tables[bctr]['r'], tables[bctr]['default']), (10, F + 0x100))

    def test_scaled_byte_offset_table_in_rdata(self):
        fixture = Fixture()
        fixture.function(F, 0x200)
        offsets, target_base = 0x82100000, F + 0x40
        oh, ol = hi_lo(offsets)
        th, tl = hi_lo(target_base)
        fixture.put(F, cmplwi(6, 11, 2), bgt(6, F + 4, F + 0x100), lis(12, oh), addi(12, 12, ol),
                    lbzx(0, 12, 11), rlwinm(0, 0, 2, 0, 29), lis(12, th), ori(0, 0, 0), addi(12, 12, tl),
                    add(12, 12, 0), mtctr(12), BCTR)
        fixture.put_bytes(offsets, [0, 3, 1])
        tables, _ = fixture.analyse()
        self.assertEqual(tables[F + 44]['labels'], [target_base, target_base + 12, target_base + 4])

    def test_index_copied_before_the_compare_is_tracked(self):
        fixture = Fixture()
        fixture.function(F, 0x200)
        table = F + 0x60
        high, low = hi_lo(table)
        # mr r28,r3 ; cmplwi r3 ; bgt ; dispatch indexes with r28.
        fixture.put(F, mr(28, 3), cmplwi(6, 3, 1), bgt(6, F + 8, F + 0x100), lis(12, high),
                    rlwinm(0, 28, 2, 0, 29), addi(12, 12, low), lwzx(0, 12, 0), mtctr(0), BCTR)
        fixture.put(table, F + 0x80, F + 0x88)
        tables, _ = fixture.analyse()
        self.assertEqual(tables[F + 32]['labels'], [F + 0x80, F + 0x88])
        self.assertIn(tables[F + 32]['r'], (3, 28))

    def test_conditional_return_bound_has_no_default_label(self):
        fixture = Fixture()
        fixture.function(F, 0x200)
        table = F + 0x40
        high, low = hi_lo(table)
        fixture.put(F, cmplwi(6, 4, 0), bgtlr(6), lis(12, high), rlwinm(0, 4, 2, 0, 29),
                    addi(12, 12, low), lwzx(0, 12, 0), mtctr(0), BCTR)
        fixture.put(table, F + 0x80)
        tables, _ = fixture.analyse()
        self.assertIsNone(tables[F + 28]['default'])
        self.assertIn('default = ', sw.to_toml([{**tables[F + 28], 'default': 1}]))
        self.assertNotIn('default', sw.to_toml([tables[F + 28]]))

    def test_label_outside_the_pdata_function_is_rejected(self):
        fixture = Fixture()
        fixture.function(F, 0x90)  # ends before the last case
        bctr, _ = absolute_switch(fixture)
        tables, rejected = fixture.analyse()
        self.assertNotIn(bctr, tables)
        self.assertIn('leaves .pdata function', rejected[bctr])

    def test_target_from_unknown_memory_is_rejected(self):
        fixture = Fixture()
        fixture.function(F, 0x200)
        fixture.put(F, cmplwi(6, 10, 2), bgt(6, F + 4, F + 0x100), lwz(12, 3, 0),
                    rlwinm(0, 10, 2, 0, 29), lwzx(0, 12, 0), mtctr(0), BCTR)
        tables, rejected = fixture.analyse()
        self.assertNotIn(F + 24, tables)
        self.assertIn('unknown register', rejected[F + 24])

    def test_index_changed_after_the_compare_is_emulated_not_assumed(self):
        fixture = Fixture()
        fixture.function(F, 0x200)
        table = F + 0x60
        high, low = hi_lo(table)
        # The compare bounds r10, but the dispatch indexes with r10 + 1.
        fixture.put(F, cmplwi(6, 10, 1), addi(10, 10, 1), bgt(6, F + 8, F + 0x100), lis(12, high),
                    rlwinm(0, 10, 2, 0, 29), addi(12, 12, low), lwzx(0, 12, 0), mtctr(0), BCTR)
        fixture.put(table, F + 0x80, F + 0x88, F + 0x90)
        tables, _ = fixture.analyse()
        self.assertEqual(tables[F + 32]['labels'], [F + 0x88, F + 0x90])

    def test_table_function_extent_covers_every_case(self):
        fixture = Fixture()
        caller = F - 0x100
        fixture.put(caller, bl(caller, F), BLR)
        bctr, cases = absolute_switch(fixture)
        fixture.function(F + 0x1000, 0x10)  # the next .pdata function bounds the search
        tables, _ = fixture.analyse()
        functions, unplaced = sw.table_functions(bytes(fixture.data), list(tables.values()))
        self.assertEqual(unplaced, [])
        self.assertEqual(functions, [{'address': F, 'size': 0x104, 'method': 'bl-target'}])
        self.assertTrue(all(F <= case < F + 0x104 for case in cases))


class JumpTableRewriteTests(unittest.TestCase):
    def emitted(self, register=10, labels=(0x82300080, 0x82300088), newline='\n'):
        body = ('\tPPC_FUNC_PROLOGUE();\n\t// bctr \n' + f'\tswitch (ctx.r{register}.u64) {{\n' +
                ''.join(f'\tcase {i}:\n\t\tgoto loc_{label:X};\n' for i, label in enumerate(labels)) +
                '\tdefault:\n\t\t__builtin_unreachable();\n\t}\n')
        return body.replace('\n', newline)

    def test_index_switch_becomes_ctr_target_dispatch(self):
        body, rewritten, invalid = rewrite_jump_tables(
            self.emitted(), 0x82300000, {0x82300000: (10, (0x82300080, 0x82300088, 0x82300080))})
        self.assertEqual((rewritten, invalid), ([], [0x82300000]))  # labels must match exactly
        body, rewritten, invalid = rewrite_jump_tables(
            self.emitted(), 0x82300000, {0x82300000: (10, (0x82300080, 0x82300088))})
        self.assertEqual((rewritten, invalid), ([0x82300000], []))
        self.assertIn('switch (ctx.ctr.u32) {', body)
        self.assertIn('case 0x82300088: goto loc_82300088;', body)
        self.assertIn('throw sfr::RuntimeStop("jump-table-target", ctx.ctr.u32', body)
        self.assertNotIn('__builtin_unreachable', body)

    def test_crlf_emission_is_preserved(self):
        body, rewritten, _ = rewrite_jump_tables(
            self.emitted(newline='\r\n'), 0x82300000, {0x82300000: (10, (0x82300080, 0x82300088))})
        self.assertEqual(rewritten, [0x82300000])
        self.assertNotIn('\n', body.replace('\r\n', ''))

    def test_wrong_register_or_error_emission_is_not_rewritten(self):
        table = {0x82300000: (11, (0x82300080, 0x82300088))}
        body, rewritten, invalid = rewrite_jump_tables(self.emitted(), 0x82300000, table)
        self.assertEqual((rewritten, invalid), ([], [0x82300000]))
        self.assertIn('__builtin_unreachable', body)


@unittest.skipUnless(os.environ.get('SFR_IMAGE_DIRECTORY'), 'requires locally prepared game image')
class ActualImageTests(unittest.TestCase):
    def test_matches_the_two_audited_local_branch_policies(self):
        data = (Path(os.environ['SFR_IMAGE_DIRECTORY']) / 'image.bin').read_bytes()
        tables, _, _ = sw.analyse(data)
        by_base = {t['base']: t for t in tables}
        for function, (branch, targets, _) in (*COUNTRY_CTR.items(), *FORMAT_CTR.items()):
            with self.subTest(function=hex(function)):
                self.assertEqual(sorted(set(by_base[branch]['labels'])), sorted(targets))
        functions, unplaced = sw.table_functions(data, tables)
        self.assertEqual(unplaced, [])
