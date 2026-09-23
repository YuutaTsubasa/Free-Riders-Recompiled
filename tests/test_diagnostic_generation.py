import hashlib
import json
from pathlib import Path
import re
import sys
import shutil
import unittest
import uuid
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import generate_diagnostic as diagnostic
try:
    from generate_diagnostic import generate
except ImportError:
    generate = None


def function(name, body):
    return (f'__attribute__((alias("__imp__{name}"))) PPC_WEAK_FUNC({name});\n'
            f'PPC_FUNC_IMPL(__imp__{name}) {{\n\tPPC_FUNC_PROLOGUE();\n{body}\n}}\n\n')


def reservation_pair(target=10, ra='0', rb=8, width=32):
    ea = ('' if ra in ('0', 'r0') else f'ctx.{ra}.u32 + ') + f'ctx.r{rb}.u32'
    body = (f'\t// lwarx r{target},{ra},r{rb}\n'
            f'\tctx.reserved.u32 = *(uint32_t*)(base + {ea});\n'
            f'\tctx.r{target}.u64 = __builtin_bswap32(ctx.reserved.u32);\n'
            f'\t// addi r{target},r{target},1\n'
            f'\tctx.r{target}.s64 = ctx.r{target}.s64 + 1;\n'
            f'\t// stwcx. r{target},{ra},r{rb}\n'
            '\tctx.cr0.lt = 0;\n\tctx.cr0.gt = 0;\n'
            f'\tctx.cr0.eq = __sync_bool_compare_and_swap(reinterpret_cast<uint32_t*>(base + {ea}), '
            f'ctx.reserved.s32, __builtin_bswap32(ctx.r{target}.s32));\n'
            '\tctx.cr0.so = ctx.xer.so;\n')
    if width == 64:
        body = body.replace('lwarx', 'ldarx').replace('stwcx.', 'stdcx.')
        body = body.replace('uint32_t', 'uint64_t').replace('bswap32', 'bswap64')
        body = body.replace('ctx.reserved.u32', 'ctx.reserved.u64').replace('ctx.reserved.s32', 'ctx.reserved.s64')
        body = body.replace(f'ctx.r{target}.s32', f'ctx.r{target}.s64')
    return body


def vector_memory(op='lvx128', reg=63, ra='r0', rb=11):
    ea = ('' if ra in ('0', 'r0') else f'ctx.{ra}.u32 + ') + f'ctx.r{rb}.u32'
    guest = f'(simde__m128i*)(base + (({ea}) & ~0xF))'
    register = f'(simde__m128i*)ctx.v{reg}.u8'
    dest, source = (register, guest) if op.startswith('l') else (guest, register)
    return (f'\t// {op} v{reg},{ra},r{rb}\n'
            f'\tsimde_mm_store_si128({dest}, simde_mm_shuffle_epi8(simde_mm_load_si128({source}), '
            'simde_mm_load_si128((simde__m128i*)VectorMaskL)));\n')


def vector_word_store(op='stvewx128', reg=43, ra='r0', rb=11):
    ea = ('' if ra in ('0', 'r0') else f'ctx.{ra}.u32 + ') + f'ctx.r{rb}.u32'
    return (f'\t// {op} v{reg},{ra},r{rb}\n'
            f'\tea = ({ea}) & ~0x3;\n'
            f'\tPPC_STORE_U32(ea, ctx.v{reg}.u32[3 - ((ea & 0xF) >> 2)]);\n')


def vector_partial_load(op='lvlx128', reg=63, ra='r0', rb=3):
    ea = ('' if ra in ('0', 'r0') else f'ctx.{ra}.u32 + ') + f'ctx.r{rb}.u32'
    left = op.startswith('lvlx')
    mask = 'L' if left else 'R'
    shuffle = ('simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)(base + '
               '(temp.u32 & ~0xF))), simde_mm_load_si128((simde__m128i*)&VectorMask' +
               mask + '[(temp.u32 & 0xF) * 16]))')
    value = shuffle if left else 'temp.u32 & 0xF ? ' + shuffle + ' : simde_mm_setzero_si128()'
    return (f'\t// {op} v{reg},{ra},r{rb}\n\ttemp.u32 = {ea};\n'
            f'\tsimde_mm_store_si128((simde__m128i*)ctx.v{reg}.u8, {value});\n')


def partial_vector_masks():
    # Fixture uses per-row slices, independently of the production validator.
    left = [0xff] * 16 + list(range(15, -1, -1))
    right = list(range(14, -1, -1)) + [0xff] * 16
    rows = {'L': [left[16-k:32-k] for k in range(16)],
            'R': [right[15-k:31-k] for k in range(16)]}
    return ''.join('inline uint8_t VectorMask' + name + '[] = {\n' +
                   ''.join(', '.join(hex(v) for v in row) + ',\n' for row in values) +
                   '};\n' for name, values in rows.items())


def vector_partial_store(op='stvlx128', reg=63, ra='r0', rb=26):
    ea = ('' if ra in ('0', 'r0') else f'ctx.{ra}.u32 + ') + f'ctx.r{rb}.u32'
    left = op.startswith('stvlx')
    count = '16 - (ea & 0xF)' if left else 'ea & 0xF'
    target = 'ea + i' if left else 'ea - i - 1'
    index = '15 - i' if left else 'i'
    return (f'\t// {op} v{reg},{ra},r{rb}\n\tea = {ea};\n'
            f'\tfor (size_t i = 0; i < ({count}); i++)\n'
            f'\t\tPPC_STORE_U8({target}, ctx.v{reg}.u8[{index}]);\n')


class DiagnosticGenerationTests(unittest.TestCase):
    @staticmethod
    def format_ctr_fixture():
        body = ('\t// lhzx r0,r12,r0\n\tctx.r0.u64 = PPC_LOAD_U16(ctx.r12.u32 + ctx.r0.u32);\n'
                '\t// mtctr r12\n\tctx.ctr.u64 = ctx.r12.u64;\n'
                '\t// bctr \n\tPPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);\n\treturn;\n')
        targets = tuple(0x100C + 4 * i for i in range(15))
        for index, target in enumerate(targets):
            if index in (1, 4, 14):
                body += f'loc_{target:X}:\n'
            body += f'\t// addi r8,r8,{index}\n\tctx.r8.s64 = ctx.r8.s64 + {index};\n'
        # These linked calls must remain calls and preserve their LR assignment.
        for index in range(3):
            body += ('\t// bctrl \n' + f'\tctx.lr = 0x{0x104C + 4 * index:X};\n'
                     '\tPPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);\n')
        body += '\t// lhzu r6,2(r30)\n\t// blr \n\treturn;'
        return body, targets

    @staticmethod
    def format_ctr_policy(body, targets, branch=0x1008):
        return {0x1000: (branch, tuple(targets),
                        hashlib.sha256(body.replace('\r\n', '\n').encode()).hexdigest())}

    def test_format_ctr_restores_local_targets_preserving_calls_table_and_lhzu(self):
        body, targets = self.format_ctr_fixture()
        self.prepare([('entry', 0x1000, body), ('companion', 0x2000, '\t// blr \n\treturn;')])
        source = self.source / 'ppc_recomp.0.cpp'
        raw = next(diagnostic.parse_functions(diagnostic.read(source), source.name))[4]
        self.log.write_text('Unrecognized instruction at 0x1054: lhzu\n')
        original_log = self.log.read_bytes()
        source.write_bytes(source.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n'))
        original = source.read_bytes()
        original_mapping = (self.source / 'ppc_func_mapping.cpp').read_bytes()
        with patch.object(diagnostic, 'FORMAT_CTR', self.format_ctr_policy(raw, targets)):
            self.assertEqual(diagnostic.validate_format_ctr(raw, 0x1000), targets)
            changed = diagnostic.rewrite_format_ctr(raw.replace('\r\n', '\n').replace('\n', '\r\n'),
                                                    0x1000, raw)
            self.assertNotIn('\n', changed.replace('\r\n', ''))
            result, report = self.run_generation()
        self.assertEqual(report['counts']['retained_functions'], 2)
        self.assertEqual(report['counts']['format_ctr_branches'], 1)
        self.assertEqual(report['counts']['country_ctr_branches'], 0)
        self.assertEqual(report['counts']['retained_lhzu'], 1)
        self.assertEqual(result.count('PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);'), 3)
        self.assertIn('PPC_LOAD_U16(ctx.r12.u32 + ctx.r0.u32)', result)
        self.assertIn('ctx.ctr.u64 = ctx.r12.u64;', result)
        for index in range(3):
            self.assertIn(f'ctx.lr = 0x{0x104C + 4 * index:X};', result)
        cases = re.findall(r'case 0x([0-9A-F]+): goto loc_([0-9A-F]+);', result)
        self.assertEqual([(int(a, 16), int(b, 16)) for a, b in cases], [(t, t) for t in targets])
        for target in targets:
            self.assertEqual(result.count(f'loc_{target:X}:'), 1)
            self.assertIn(f'loc_{target:X}:\n\tsfr::guest_checkpoint();', result)
        self.assertIn('default: throw sfr::RuntimeStop("format-ctr-target", ctx.ctr.u32, '
                      '"unrecognized local format branch target");', result)
        self.assertEqual([m[1].rstrip('\r') for m in diagnostic.INSTRUCTION.finditer(result)],
                         [m[1].rstrip('\r') for m in diagnostic.INSTRUCTION.finditer(diagnostic.read(source))])
        self.assertEqual(source.read_bytes(), original)
        self.assertEqual(self.log.read_bytes(), original_log)
        self.assertEqual((self.output / 'ppc_func_mapping.cpp').read_bytes(), original_mapping)

    def test_format_ctr_original_and_emission_hash_changes_reject(self):
        body, targets = self.format_ctr_fixture()
        with patch.object(diagnostic, 'FORMAT_CTR', self.format_ctr_policy(body, targets)):
            for changed in [body.replace('ctx.r8.s64 + 0', 'ctx.r8.s64 + 1', 1),
                            body.replace('// lhzx r0,r12,r0', '// lhzx r0,r12,r1'),
                            body.replace('ctx.lr = 0x104C;', 'ctx.lr = 0x0;')]:
                with self.subTest(changed=changed[:100]):
                    with self.assertRaises(ValueError):
                        diagnostic.validate_format_ctr(changed, 0x1000)
                    with self.assertRaises(ValueError):
                        diagnostic.rewrite_format_ctr(changed, 0x1000, body)
                    _, reasons, _ = diagnostic.inspect_body(changed, 0x1000, {'entry'}, {})
                    self.assertIn('unsupported_format_ctr', reasons)

    def test_format_ctr_rehashed_malformed_blocks_labels_and_policies_reject(self):
        body, targets = self.format_ctr_fixture()
        label = f'loc_{targets[-1]:X}:'
        changes = [body.replace('// bctr ', '// bctrl ', 1),
                   body.replace('\treturn;\n', '', 1),
                   body.replace('\treturn;\n', '\treturn;\n\tctx.r3.u64 = 1;\n', 1),
                   body.replace('// bctrl ', '// bctr ', 1),
                   body.replace(label + '\n', label + ' \n'),
                   body.replace(label + '\n', label + ' ctx.r3.u64 = 1;\n'),
                   body.replace(label + '\n', label + '\n\tctx.r3.u64 = 1;\n'),
                   body.replace(label + '\n', label + '\n' + label + '\n'),
                   body.replace(label + '\n\t// addi r8,r8,14\n',
                                '\t// addi r8,r8,14\n' + label + '\n')]
        for changed in changes:
            with self.subTest(changed=changed[:100]), patch.object(
                    diagnostic, 'FORMAT_CTR', self.format_ctr_policy(changed, targets)):
                with self.assertRaises(ValueError):
                    diagnostic.validate_format_ctr(changed, 0x1000)
        for branch, bad_targets in [(0x1009, targets), (0x1004, targets),
                                    (0x1008, (targets[0] + 1,) + targets[1:]),
                                    (0x1008, targets + (targets[0],)),
                                    (0x1008, (0x2000,) + targets[1:]),
                                    (0x1008, (0x1004,) + targets[1:])]:
            with self.subTest(branch=branch, targets=bad_targets), patch.object(
                    diagnostic, 'FORMAT_CTR', self.format_ctr_policy(body, bad_targets, branch)):
                with self.assertRaises(ValueError):
                    diagnostic.validate_format_ctr(body, 0x1000)

    def test_format_ctr_leaves_unrelated_logs_and_functions_unchanged(self):
        body, targets = self.format_ctr_fixture()
        self.prepare([('entry', 0x1000, body)])
        source = self.source / 'ppc_recomp.0.cpp'
        raw = next(diagnostic.parse_functions(diagnostic.read(source), source.name))[4]
        self.log.write_text('Unrecognized instruction at 0x1054: lhzu\n'
                            'Unrecognized instruction at 0x1008: unknown\n')
        with patch.object(diagnostic, 'FORMAT_CTR', self.format_ctr_policy(raw, targets)):
            self.assertIsNone(diagnostic.validate_format_ctr(raw, 0x2000))
            self.assertEqual(diagnostic.rewrite_format_ctr(raw, 0x2000, raw), raw)
            result, report = self.run_generation()
        self.assertEqual(report['counts']['format_ctr_branches'], 0)
        self.assertEqual(report['counts']['retained_functions'], 0)
        self.assertIn('logged_unsupported_instruction', report['rejected_functions'][0]['reasons'])
        self.assertNotIn('switch (ctx.ctr.u32)', result)

    @staticmethod
    def country_ctr_fixture():
        targets = tuple(0x1008 + 8 * i for i in range(38))
        ending = targets[-1] + 4
        body = ('\t// mtctr r11\n\tctx.ctr.u64 = ctx.r11.u64;\n'
                '\t// bctr \n\tPPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);\n\treturn;\n')
        for index, target in enumerate(targets):
            if index == 37:
                body += f'loc_{target:X}:\n'
            value = index + 1 if index < 37 else 0
            body += f'\t// li r11,{value}\n\tctx.r11.s64 = {value};\n'
            if index < 37:
                body += f'\t// b 0x{ending:x}\n\tgoto loc_{ending:X};\n'
        body += f'loc_{ending:X}:\n\t// blr \n\treturn;'
        return body, targets

    @staticmethod
    def country_ctr_policy(body, targets, branch=0x1004):
        digest = hashlib.sha256(body.replace('\r\n', '\n').encode()).hexdigest()
        return {0x1000: (branch, tuple(targets), digest)}

    def test_country_ctr_restores_38_exact_local_targets_and_checkpoints(self):
        body, targets = self.country_ctr_fixture()
        companion = '\t// li r3,17\n\tctx.r3.s64 = 17;\n\t// blr \n\treturn;'
        self.prepare([('entry', 0x1000, body), ('companion', 0x2000, companion)])
        source = self.source / 'ppc_recomp.0.cpp'
        raw = next(diagnostic.parse_functions(diagnostic.read(source), source.name))[4]
        raw = raw.replace('\r\n', '\n')
        with patch.object(diagnostic, 'COUNTRY_CTR', {}):
            prior, prior_report = self.run_generation()
        shutil.rmtree(self.output)
        source.write_bytes(source.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n'))
        original = source.read_bytes()
        with patch.object(diagnostic, 'COUNTRY_CTR', self.country_ctr_policy(raw, targets)):
            self.assertEqual(diagnostic.validate_country_ctr(raw, 0x1000), targets)
            rewritten = diagnostic.rewrite_country_ctr(raw.replace('\n', '\r\n'), 0x1000,
                                                        raw.replace('\n', '\r\n'))
            self.assertNotIn('\n', rewritten.replace('\r\n', ''))
            result, report = self.run_generation()
        self.assertEqual(prior_report['counts']['retained_functions'], 2)
        self.assertEqual(report['counts']['retained_functions'], 2)
        self.assertEqual(report['counts']['country_ctr_branches'], 1)
        self.assertEqual(report['rejected_functions'], prior_report['rejected_functions'])
        emitted = re.findall(r'case 0x([0-9A-F]+): goto loc_([0-9A-F]+);', result)
        self.assertEqual([(int(case, 16), int(label, 16)) for case, label in emitted],
                         [(target, target) for target in targets])
        for target in targets:
            self.assertEqual(result.count(f'loc_{target:X}:'), 1)
            self.assertIn(f'loc_{target:X}:\n\tsfr::guest_checkpoint();', result)
        self.assertIn(f'loc_{targets[-1] + 4:X}:\n\tsfr::guest_checkpoint();', result)
        self.assertIn('default: throw sfr::RuntimeStop("country-ctr-target", ctx.ctr.u32, '
                      '"unrecognized local country branch target");', result)
        self.assertNotIn('PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);', result)
        self.assertIn('ctx.ctr.u64 = ctx.r11.u64;', result)
        self.assertEqual([m[1].rstrip('\r') for m in diagnostic.INSTRUCTION.finditer(result)],
                         [m[1].rstrip('\r') for m in diagnostic.INSTRUCTION.finditer(prior)])
        self.assertEqual(result.split('PPC_FUNC_IMPL(__imp__companion)', 1)[1],
                         prior.split('PPC_FUNC_IMPL(__imp__companion)', 1)[1])
        self.assertEqual(source.read_bytes(), original)

    def test_country_ctr_hash_changes_fail_before_any_restoration(self):
        body, targets = self.country_ctr_fixture()
        changes = [body.replace('ctx.r11.s64 = 1;', 'ctx.r11.s64 = 2;', 1),
                   body.replace('// mtctr r11', '// mtctr r12', 1),
                   body.replace('\t// bctr ', '\tctx.r3.u64 = 9;\n\t// bctr ', 1)]
        with patch.object(diagnostic, 'COUNTRY_CTR', self.country_ctr_policy(body, targets)):
            for changed in changes:
                with self.subTest(changed=changed[:100]):
                    with self.assertRaises(ValueError):
                        diagnostic.validate_country_ctr(changed, 0x1000)
                    with self.assertRaises(ValueError):
                        diagnostic.rewrite_country_ctr(changed, 0x1000, changed)
                    _, reasons, _ = diagnostic.inspect_body(changed, 0x1000, {'entry'}, {})
                    self.assertIn('unsupported_country_ctr', reasons)

    def test_country_ctr_rehashed_noncanonical_blocks_labels_and_policies_reject(self):
        body, targets = self.country_ctr_fixture()
        label = f'loc_{targets[-1]:X}:'
        changes = [
            body.replace('PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);',
                         'PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);\n\tctx.r3.u64 = 9;', 1),
            body.replace('\treturn;\n', '', 1),
            body.replace('// bctr ', '// bctrl ', 1),
            body.replace(label + '\n', label + ' \n'),
            body.replace(label + '\n', label + '\t\n'),
            body.replace(label + '\n', label + ' ctx.r3.u64 = 9;\n'),
            body.replace(label + '\n', label + '\n\tctx.r3.u64 = 9;\n'),
            body.replace(label + '\n', label + '\n' + label + '\n'),
            body.replace(label + '\n\t// li r11,0\n', '\t// li r11,0\n' + label + '\n'),
            body.replace('ctx.r11.s64 = 1;', 'ctx.r11.u64 = 1;', 1),
            body.replace('// li r11,1\n', '// li r12,1\n', 1),
        ]
        for changed in changes:
            with self.subTest(changed=changed[:100]), patch.object(
                    diagnostic, 'COUNTRY_CTR', self.country_ctr_policy(changed, targets)):
                with self.assertRaises(ValueError):
                    diagnostic.validate_country_ctr(changed, 0x1000)
                _, reasons, _ = diagnostic.inspect_body(changed, 0x1000, {'entry'}, {})
                self.assertIn('unsupported_country_ctr', reasons)
        for branch, invalid_targets in [(0x1005, targets), (0x1000, targets),
                                        (0x1004, (targets[0] + 1,) + targets[1:]),
                                        (0x1004, targets + (targets[0],)),
                                        (0x1004, (0x2000,) + targets[1:])]:
            with self.subTest(branch=branch, targets=invalid_targets), patch.object(
                    diagnostic, 'COUNTRY_CTR', self.country_ctr_policy(body, invalid_targets, branch)):
                with self.assertRaises(ValueError):
                    diagnostic.validate_country_ctr(body, 0x1000)

    def test_country_ctr_rewrite_revalidates_emission_and_preserves_unrelated_rejections(self):
        body, targets = self.country_ctr_fixture()
        self.prepare([('entry', 0x1000, body)])
        source = self.source / 'ppc_recomp.0.cpp'
        raw = next(diagnostic.parse_functions(diagnostic.read(source), source.name))[4]
        raw = raw.replace('\r\n', '\n')
        self.log.write_text('Unrecognized instruction at 0x1000: unknown\n')
        with patch.object(diagnostic, 'COUNTRY_CTR', self.country_ctr_policy(raw, targets)):
            for changed in [raw.replace('// li r11,1\n', '// li r11,2\n', 1),
                            raw.replace('PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);',
                                        'PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);\n\tctx.r3.u64 = 0;', 1),
                            raw.replace('loc_1130:\n', 'loc_1130: \n')]:
                with self.subTest(changed=changed[:100]), self.assertRaises(ValueError):
                    diagnostic.rewrite_country_ctr(changed, 0x1000, raw)
            self.assertIsNone(diagnostic.validate_country_ctr(raw, 0x2000))
            self.assertEqual(diagnostic.rewrite_country_ctr(raw, 0x2000, raw), raw)
            result, report = self.run_generation()
        self.assertEqual(report['counts']['country_ctr_branches'], 0)
        self.assertEqual(report['counts']['retained_functions'], 0)
        self.assertIn('logged_unsupported_instruction', report['rejected_functions'][0]['reasons'])
        self.assertNotIn('switch (ctx.ctr.u32)', result)

    def test_partial_vector_loads_rewrite_four_variants_preserving_ea_and_checkpoints(self):
        body = (vector_partial_load() + vector_partial_load('lvrx128', 127, 'r31', 0) +
                vector_partial_load('lvlx', 31, '0', 31) + vector_partial_load('lvrx', 0, 'r3', 3) +
                'loc_1010:\n\t// blr \n\treturn;')
        self.prepare([('entry', 0x1000, body)])
        (self.source / 'ppc_context.h').write_text(partial_vector_masks())
        source = self.source / 'ppc_recomp.0.cpp'
        source.write_bytes(source.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n'))
        originals = {p: p.read_bytes() for p in [source, self.log, self.source / 'ppc_context.h']}
        result, report = self.run_generation()
        self.assertEqual(report['counts']['retained_functions'], 1)
        self.assertEqual(report['counts']['vector_partial_loads'], 4)
        for hook, reg, ea in [('left', 63, 'ctx.r3.u32'), ('right', 127, 'ctx.r31.u32 + ctx.r0.u32'),
                              ('left', 31, 'ctx.r31.u32'), ('right', 0, 'ctx.r3.u32 + ctx.r3.u32')]:
            self.assertIn(f'temp.u32 = {ea};\n\tsfr::load_vector_{hook}(temp.u32, ctx.v{reg}.u8);', result)
        self.assertIn('loc_1010:\n\tsfr::guest_checkpoint();', result)
        self.assertEqual(len(list(diagnostic.INSTRUCTION.finditer(result))), 5)
        self.assertNotIn('simde_mm_', result)
        for path, original in originals.items():
            self.assertEqual(path.read_bytes(), original)
        raw = next(diagnostic.parse_functions(diagnostic.read(source), source.name))[4]
        rewritten, count, invalid = diagnostic.rewrite_vector_partial_loads(raw, 0x1000)
        self.assertEqual((count, invalid), (4, []))
        self.assertIn('ctx.v63.u8);\r\n', rewritten)

    def test_partial_vector_loads_reject_malformed_annotations_and_complete_emissions(self):
        canonical = vector_partial_load() + '\t// blr \n\treturn;'
        right = vector_partial_load('lvrx128') + '\t// blr \n\treturn;'
        cases = [vector_partial_load('lvlx', 32), vector_partial_load('lvrx128', 128),
                 canonical.replace('v63,', 'v063,'), canonical.replace('r0,r3', 'r00,r3'),
                 canonical.replace('r0,r3', 'r32,r3'), canonical.replace('r0,r3', 'r0,r32'),
                 canonical.replace('lvlx128', 'lvlxl128'), canonical.replace('lvlx128', 'LVLX128'),
                 canonical.replace('lvlx128', 'lvlx128.'), canonical.replace('temp.u32 =', 'temp.u64 ='),
                 canonical.replace('= ctx.r3.u32;', '= ctx.r0.u32 + ctx.r3.u32;'),
                 canonical.replace('ctx.v63.u8', 'ctx.v62.u8'), canonical.replace('~0xF', '~0x7'),
                 canonical.replace('VectorMaskL', 'VectorMaskR'), canonical.replace('* 16', '* 8'),
                 right.replace('temp.u32 & 0xF ? ', ''), right.replace('simde_mm_setzero_si128()', 'ctx.v0'),
                 canonical.replace('\t// blr', '\tctx.r3.u64 = 0;\n\t// blr'),
                 canonical.replace('\t// blr', 'loc_1004:\n\tctx.r3.u64 = 0;\n\t// blr'),
                 canonical.replace('\tsimde_mm_store', 'loc_1000:\n\tsimde_mm_store'),
                 '\t// lvlx128 v63,r0,r3\n']
        for index, body in enumerate(cases):
            with self.subTest(index=index):
                _, reasons, details = diagnostic.inspect_body(body, 0x1000, {'entry'}, {})
                self.assertIn('unsupported_vector_partial_load', reasons)
                self.assertEqual(details['invalid_vector_partial_load_addresses'], ['0x00001000'])
        self.prepare([('entry', 0x1000, canonical)])
        self.log.write_text('Unrecognized instruction at 0x1000: unrelated\n')
        result, report = self.run_generation()  # Rejected functions do not require masks.
        self.assertEqual(report['counts']['retained_functions'], 0)
        self.assertEqual(report['counts'].get('vector_partial_loads'), 0)
        self.assertIn('logged_unsupported_instruction', report['rejected_functions'][0]['reasons'])
        self.assertNotIn('sfr::load_vector_left', result)

    def test_partial_vector_loads_require_all_512_pinned_mask_bytes(self):
        validate = getattr(diagnostic, 'validate_vector_partial_masks', None)
        self.assertTrue(callable(validate), 'complete partial-vector mask validator missing')
        header = partial_vector_masks()
        validate(header)
        tokens = list(re.finditer(r'0x[0-9a-f]+', header))
        self.assertEqual(len(tokens), 512)
        for index, token in enumerate(tokens):
            with self.subTest(index=index), self.assertRaisesRegex(ValueError, 'VectorMask'):
                changed = hex(int(token[0], 16) ^ 1)
                validate(header[:token.start()] + changed + header[token.end():])
        malformed = [header + header, header.replace('VectorMaskR', 'OtherMask'),
                     header.replace('0xff,', '', 1), header.replace('};', ', 0xff};', 1),
                     header.replace(',\n};', ',,\n};', 1),
                     header + 'inline uint8_t VectorMaskR[256] = {};\n',
                     header.replace('0xff', '0377', 1), header.replace('0xff', '(255)', 1),
                     header.replace('0xff', '0x100', 1)]
        for changed in malformed:
            with self.subTest(header=changed[:60]), self.assertRaisesRegex(ValueError, 'VectorMask'):
                validate(changed)

    def test_partial_vector_loads_validate_masks_only_before_retained_publication(self):
        self.prepare([('entry', 0x1000, vector_partial_load() + '\t// blr \n\treturn;')])
        for index, header in enumerate(['', partial_vector_masks().split('inline uint8_t VectorMaskR')[0],
                                         partial_vector_masks().replace('0xe, 0xd', '0xd, 0xe', 1)]):
            with self.subTest(index=index):
                (self.source / 'ppc_context.h').write_text(header)
                self.output = self.root / f'bad-partial-mask-{index}'
                with self.assertRaisesRegex(ValueError, 'VectorMask'):
                    generate(self.source, self.log, self.output)
                self.assertFalse(self.output.exists())
        self.prepare([('entry', 0x1000, vector_memory() + '\t// blr \n\treturn;')])
        self.vector_mask()  # Legacy full-vector-only code still needs only its first row.
        self.output = self.root / 'legacy-full-mask'
        _, report = self.run_generation()
        self.assertEqual(report['counts']['retained_functions'], 1)

    def test_partial_vector_stores_rewrite_full_canonical_blocks(self):
        body = (vector_partial_store() + vector_partial_store('stvrx128', 127, 'r31', 0) +
                vector_partial_store('stvlx', 31, '0', 2) + vector_partial_store('stvrx', 0, 'r1', 31) +
                'loc_1010:\n\t// blr \n\treturn;')
        self.prepare([('entry', 0x1000, body)])
        self.vector_mask()
        output, report = self.run_generation()
        self.assertEqual(report['counts']['retained_functions'], 1)
        self.assertEqual(report['counts']['vector_partial_stores'], 4)
        self.assertIn('sfr::store_vector_left(ea, ctx.v63.u8);', output)
        self.assertIn('sfr::store_vector_right(ea, ctx.v127.u8);', output)
        self.assertIn('ea = ctx.r31.u32 + ctx.r0.u32;', output)
        self.assertIn('loc_1010:', output)
        self.assertNotIn('for (size_t i', output)
        self.assertIn('PPC_STORE_U8', (self.source / 'ppc_recomp.0.cpp').read_text())

    def test_partial_vector_stores_reject_variants_and_extra_statements(self):
        canonical = vector_partial_store() + '\t// blr \n\treturn;'
        cases = [vector_partial_store('stvlx', 32), vector_partial_store('stvrx128', 128),
                 canonical.replace('v63,', 'v063,'), canonical.replace('r0,r26', 'r00,r26'),
                 canonical.replace('15 - i', 'i'), canonical.replace('ea + i', 'ea - i'),
                 canonical.replace('16 - (ea & 0xF)', '15 - (ea & 0xF)'),
                 canonical.replace('\t// blr', '\tctx.r3.u64 = 0;\n\t// blr'),
                 canonical.replace('\t// blr', 'loc_1004:\n\tctx.r3.u64 = 0;\n\t// blr'),
                 canonical.replace('stvlx128', 'stvlx128.'), canonical.replace('stvlx128', 'STVLX128')]
        self.prepare([(f'entry{i}', 0x1000 + i * 0x100, body) for i, body in enumerate(cases)])
        _, report = self.run_generation()
        self.assertEqual(report['counts']['retained_functions'], 0)

    def test_barriers_become_fences_and_keep_their_comments(self):
        body = ('\t// lwsync \n\t// sync \n\t// eieio \n'
                '\t// addi r3,r3,1\n\tctx.r3.s64 = ctx.r3.s64 + 1;\n\t// blr \n\treturn;')
        self.prepare([('entry', 0x1000, body)])
        output, report = self.run_generation()
        self.assertEqual(report['counts']['retained_functions'], 1)
        self.assertEqual(report['counts']['barriers'], 3)
        self.assertIn('\t// lwsync \n\tstd::atomic_thread_fence(std::memory_order_acq_rel);', output)
        self.assertIn('\t// sync \n\tstd::atomic_thread_fence(std::memory_order_seq_cst);', output)
        self.assertIn('\t// eieio \n\tstd::atomic_thread_fence(std::memory_order_acq_rel);', output)

    def test_barrier_with_an_emission_of_its_own_is_rejected(self):
        body = '\t// sync \n\tsome_other_emission();\n\t// blr \n\treturn;'
        self.prepare([('entry', 0x1000, body)])
        _, report = self.run_generation()
        self.assertEqual(report['counts']['retained_functions'], 0)
        self.assertIn('unsupported_barrier', report['rejected_functions'][0]['reasons'])

    def test_dcbzl_rewrites_exact_128_byte_blocks(self):
        body = '\t// dcbzl r4,r31\n\tmemset(base + ((ctx.r4.u32 + ctx.r31.u32) & ~127), 0, 128);\n\t// blr \n\treturn;'
        self.prepare([('entry', 0x1000, body)])
        output, report = self.run_generation()
        self.assertEqual(report['counts']['retained_functions'], 1)
        self.assertEqual(report['counts']['cache_line_zeroes'], 1)
        self.assertEqual(report['counts']['cache_block_zeroes'], 0)
        self.assertIn('sfr::zero_cache_line(uint32_t(ctx.r4.u32 + ctx.r31.u32));',
                      output)

    def test_dcbzl_rejects_wrong_width_and_extra_emissions(self):
        body = '\t// dcbzl r0,r3\n\tmemset(base + ((ctx.r3.u32) & ~127), 0, 128);\n\t// blr \n\treturn;'
        cases = [body.replace('128);', '32);'), body.replace('~127', '~31'),
                 body.replace('\t// blr', 'loc_1004:\n\tctx.r3.u64 = 0;\n\t// blr'),
                 body.replace('dcbzl ', 'dcbzl128 '), body.replace('dcbzl ', 'DCBZL ')]
        self.prepare([(f'entry{i}', 0x1000 + i * 0x100, item) for i, item in enumerate(cases)])
        _, report = self.run_generation()
        self.assertEqual(report['counts']['retained_functions'], 0)

    @staticmethod
    def coherency_fixture():
        return ('\t// addc r3,r4,r5\n'
                '\t// li r3,1\n\tctx.r3.u64 = 1;\n'
                'loc_1008:\n\t// b 0x100c\n\tgoto loc_100C;\n'
                'loc_100C:\n\t// blr \n\treturn;')

    @staticmethod
    def coherency_policy(body, begin=0x1004, end=0x100C):
        digest = hashlib.sha256(body.replace('\r\n', '\n').encode()).hexdigest()
        return {0x1000: (begin, end, digest)}

    def test_native_coherency_emission_preserves_comments_cpu_rewrites_and_end_checkpoint(self):
        self.prepare([('entry', 0x1000, self.coherency_fixture())])
        source = self.source / 'ppc_recomp.0.cpp'
        raw = next(diagnostic.parse_functions(diagnostic.read(source), source.name))[4]
        policy = self.coherency_policy(raw)
        source.write_bytes(source.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n'))
        original = source.read_bytes()
        self.log.write_text('Unrecognized instruction at 0x1000: addc\n')
        with patch.object(diagnostic, 'NATIVE_RESOURCE_COHERENCY', policy):
            result, report = self.run_generation()
        self.assertEqual(report['counts']['native_resource_coherency_blocks'], 1)
        self.assertEqual(report['counts']['retained_addc'], 1)
        self.assertEqual(result.count('sfr::synchronize_resource_memory(ctx);'), 1)
        self.assertIn('sfr::addc(ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, ctx.xer.ca);', result)
        self.assertNotIn('ctx.r3.u64 = 1;', result)
        self.assertNotIn('loc_1008:', result)
        self.assertNotIn('goto loc_100C;', result)
        self.assertIn('loc_100C:\n\tsfr::guest_checkpoint();', result)
        self.assertIn('\treturn;', result)
        self.assertEqual([m[1].rstrip('\r') for m in diagnostic.INSTRUCTION.finditer(result)],
                         [m[1].rstrip('\r') for m in diagnostic.INSTRUCTION.finditer(raw)])
        self.assertEqual(source.read_bytes(), original)

    def test_native_coherency_whole_function_hash_mismatch_rejects_before_emission(self):
        body = self.coherency_fixture()
        with patch.object(diagnostic, 'NATIVE_RESOURCE_COHERENCY', self.coherency_policy(body)):
            for changed in (body.replace('r4,r5', 'r4,r6'), body.replace('= 1;', '= 2;'),
                            body.replace('\treturn;', '\tctx.r8.u64 = 7;\n\treturn;')):
                with self.subTest(body=changed):
                    _, reasons, _ = diagnostic.inspect_body(changed, 0x1000, {'entry'}, {})
                    self.assertIn('unsupported_native_resource_coherency', reasons)
                    with self.assertRaises(ValueError):
                        diagnostic.rewrite_native_resource_coherency(changed, 0x1000, changed)

    def test_native_coherency_rejects_external_entries_exits_and_invalid_end_labels(self):
        canonical = self.coherency_fixture()
        cases = [canonical.replace('\t// addc r3,r4,r5\n', '\t// b 0x1008\n\tgoto loc_1008;\n'),
                 canonical.replace('goto loc_100C;', 'goto loc_1000;'),
                 canonical.replace('loc_100C:\n', ''),
                 canonical.replace('loc_100C:', 'loc_100C: '),
                 canonical.replace('loc_100C:', 'loc_100C: ctx.r8.u64 = 1;'),
                 canonical.replace('loc_100C:\n\t// blr ', '\t// blr \nloc_100C:')]
        for body in cases:
            with self.subTest(body=body), patch.object(diagnostic, 'NATIVE_RESOURCE_COHERENCY', self.coherency_policy(body)):
                _, reasons, _ = diagnostic.inspect_body(body, 0x1000, {'entry'}, {})
                self.assertIn('unsupported_native_resource_coherency', reasons)
        for begin, end in ((0x1005, 0x100C), (0x100C, 0x1004), (0x1004, 0x1010)):
            with self.subTest(begin=begin, end=end), patch.object(
                    diagnostic, 'NATIVE_RESOURCE_COHERENCY', self.coherency_policy(canonical, begin, end)):
                with self.assertRaises(ValueError):
                    diagnostic.validate_native_resource_coherency(canonical, 0x1000)

    def test_native_coherency_keeps_unsupported_events_and_other_functions_unchanged(self):
        self.prepare([('entry', 0x1000, self.coherency_fixture())])
        source = self.source / 'ppc_recomp.0.cpp'
        raw = next(diagnostic.parse_functions(diagnostic.read(source), source.name))[4]
        self.log.write_text('Unrecognized instruction at 0x1000: addc\n'
                            'Unrecognized instruction at 0x1004: unknown\n')
        with patch.object(diagnostic, 'NATIVE_RESOURCE_COHERENCY', self.coherency_policy(raw)):
            result, report = self.run_generation()
            self.assertEqual(diagnostic.rewrite_native_resource_coherency(raw, 0x2000, raw), raw)
        self.assertEqual(report['counts']['native_resource_coherency_blocks'], 0)
        self.assertIn('logged_unsupported_instruction', report['rejected_functions'][0]['reasons'])
        self.assertNotIn('sfr::synchronize_resource_memory(ctx);', result)

    def test_addme_logged_empty_blocks_preserve_sources_aliases_and_labels(self):
        registers = [(0, 0), (31, 1), (10, 10), (1, 0), (0, 31)]
        body = ''.join(f'\t// addme r{d},r{a}\n' + ('loc_1004:\n' if index == 0 else '')
                       for index, (d, a) in enumerate(registers))
        body += '\t// blr \n\treturn;'
        self.prepare([('entry', 0x1000, body)])
        source = self.source / 'ppc_recomp.0.cpp'
        source.write_bytes(source.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n'))
        original = source.read_bytes()
        self.log.write_text(''.join(f'Unrecognized instruction at 0x{0x1000+i*4:X}: addme\n' for i in range(5)) +
                            'Unrecognized instruction at 0x1000: addme\n')
        result, report = self.run_generation()
        self.assertEqual(report['counts']['retained_addme'], 5)
        self.assertEqual(report['counts']['retained_functions'], 1)
        for d, a in registers:
            self.assertIn(f'sfr::addme(ctx.r{d}.u64, ctx.r{a}.u64, ctx.xer.ca);', result)
        self.assertIn('ctx.xer.ca);\nloc_1004:\n\tsfr::guest_checkpoint();', result)
        self.assertEqual(source.read_bytes(), original)

    def test_addme_rejects_variants_nonempty_blocks_and_conflicting_logs(self):
        canonical = '\t// addme r10,r10\n\t// blr \n\treturn;'
        cases = [canonical.replace('addme ', variant + ' ') for variant in ('addme.', 'addmeo', 'addmeo.', 'ADDME')]
        cases += [canonical.replace('r10,r10', operands) for operands in
                  ('r32,r10', 'r10,r32', 'r010,r10', 'r10,0', 'r10, r10', 'r10,r10,r5')]
        cases += [canonical.replace('\t// blr', code + '\t// blr') for code in
                  ('\tctx.r10.u64 = 0;\n', 'loc_1004:\n\tctx.r10.u64 = 0;\n',
                   'loc_1004: ctx.xer.ca = 0;\n')]
        events = {0x1000: [('unrecognized', 'addme')]}
        for body in cases:
            with self.subTest(body=body):
                _, reasons, _ = diagnostic.inspect_body(body, 0x1000, {'entry'}, events)
                self.assertIn('unsupported_addme', reasons)
        for bad_events in ({}, {0x1000: [('unrecognized', 'addme'), ('decode', None)]},
                           {0x1000: [('unrecognized', 'addmeo')]}):
            with self.subTest(events=bad_events):
                _, reasons, _ = diagnostic.inspect_body(canonical, 0x1000, {'entry'}, bad_events)
                self.assertIn('unsupported_addme', reasons)

    def test_addme_resolution_preserves_unrelated_instruction_events(self):
        body = '\t// addme r0,r31\n\t// nop \n\t// blr \n\treturn;'
        events = {0x1000: [('unrecognized', 'addme')], 0x1004: [('unrecognized', 'unknown')]}
        _, reasons, details = diagnostic.inspect_body(body, 0x1000, {'entry'}, events)
        self.assertEqual(details['retained_addme'], 1)
        self.assertIn('logged_unsupported_instruction', reasons)
        self.assertEqual(details['unsupported_addresses'], ['0x00001004'])
        _, reasons, _ = diagnostic.inspect_body(body.replace('addme r0,r31', 'nop '), 0x1000, {'entry'}, events)
        self.assertIn('unsupported_addme', reasons)

    def test_addc_logged_empty_blocks_preserve_sources_aliases_and_labels(self):
        registers = [(0, 0, 0), (31, 1, 2), (10, 10, 5), (11, 5, 11), (5, 10, 11)]
        body = ''.join(f'\t// addc r{d},r{a},r{b}\n' + ('loc_1004:\n' if index == 0 else '')
                       for index, (d, a, b) in enumerate(registers))
        body += '\t// blr \n\treturn;'
        self.prepare([('entry', 0x1000, body)])
        source = self.source / 'ppc_recomp.0.cpp'
        source.write_bytes(source.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n'))
        original = source.read_bytes()
        self.log.write_text(''.join(f'Unrecognized instruction at 0x{0x1000+i*4:X}: addc\n' for i in range(5)) +
                            'Unrecognized instruction at 0x1000: addc\n')
        result, report = self.run_generation()
        self.assertEqual(report['counts']['retained_addc'], 5)
        self.assertEqual(report['counts']['retained_functions'], 1)
        for d, a, b in registers:
            self.assertIn(f'sfr::addc(ctx.r{d}.u64, ctx.r{a}.u64, ctx.r{b}.u64, ctx.xer.ca);', result)
        self.assertIn('ctx.xer.ca);\nloc_1004:\n\tsfr::guest_checkpoint();', result)
        self.assertEqual(source.read_bytes(), original)

    def test_addc_rejects_variants_nonempty_blocks_and_conflicting_logs(self):
        canonical = '\t// addc r10,r10,r5\n\t// blr \n\treturn;'
        cases = [canonical.replace('addc ', variant + ' ') for variant in ('addc.', 'addco', 'addco.', 'ADDC')]
        cases += [canonical.replace('r10,r10,r5', operands) for operands in
                  ('r32,r10,r5', 'r10,r32,r5', 'r10,r10,r32', 'r010,r10,r5', 'r10,0,r5', 'r10, r10,r5')]
        cases += [canonical.replace('\t// blr', code + '\t// blr') for code in
                  ('\tctx.r10.u64 = 0;\n', 'loc_1004:\n\tctx.r10.u64 = 0;\n',
                   'loc_1004: ctx.xer.ca = 0;\n')]
        events = {0x1000: [('unrecognized', 'addc')]}
        for body in cases:
            with self.subTest(body=body):
                _, reasons, _ = diagnostic.inspect_body(body, 0x1000, {'entry'}, events)
                self.assertIn('unsupported_addc', reasons)
        for bad_events in ({}, {0x1000: [('unrecognized', 'addc'), ('decode', None)]},
                           {0x1000: [('unrecognized', 'addco')]}):
            with self.subTest(events=bad_events):
                _, reasons, _ = diagnostic.inspect_body(canonical, 0x1000, {'entry'}, bad_events)
                self.assertIn('unsupported_addc', reasons)

    def test_addc_resolution_preserves_unrelated_instruction_events(self):
        body = '\t// addc r0,r0,r31\n\t// nop \n\t// blr \n\treturn;'
        events = {0x1000: [('unrecognized', 'addc')], 0x1004: [('unrecognized', 'unknown')]}
        _, reasons, details = diagnostic.inspect_body(body, 0x1000, {'entry'}, events)
        self.assertEqual(details['retained_addc'], 1)
        self.assertIn('logged_unsupported_instruction', reasons)
        self.assertEqual(details['unsupported_addresses'], ['0x00001004'])
        _, reasons, _ = diagnostic.inspect_body(body.replace('addc r0,r0,r31', 'nop '), 0x1000, {'entry'}, events)
        self.assertIn('unsupported_addc', reasons)

    def test_dcbz_exact_memory_blocks_preserve_operands_labels_and_input(self):
        pairs = [('r0', 0), ('0', 31), ('r1', 0), ('r31', 31)]
        body = ''
        for ra, rb in pairs:
            ea = ('' if ra in ('0', 'r0') else f'ctx.{ra}.u32 + ') + f'ctx.r{rb}.u32'
            body += f'\t// dcbz {ra},r{rb}\n\tmemset(base + (({ea}) & ~31), 0, 32);\n'
        body += 'loc_1010:\n\t// blr \n\treturn;'
        self.prepare([('entry', 0x1000, body)])
        source = self.source / 'ppc_recomp.0.cpp'
        source.write_bytes(source.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n'))
        original = source.read_bytes()
        result, report = self.run_generation()
        self.assertEqual(report['counts']['cache_block_zeroes'], 4)
        self.assertEqual(report['counts']['retained_functions'], 1)
        for ra, rb in pairs:
            ea = ('' if ra in ('0', 'r0') else f'ctx.{ra}.u32 + ') + f'ctx.r{rb}.u32'
            self.assertIn(f'sfr::zero_cache_block(uint32_t({ea}));', result)
        self.assertIn('loc_1010:\n\tsfr::guest_checkpoint();', result)
        self.assertNotIn('memset(base', result)
        self.assertEqual(source.read_bytes(), original)

    def test_dcbz_rejects_variants_mismatched_or_extra_emissions(self):
        canonical = '\t// dcbz r0,r11\n\tmemset(base + ((ctx.r11.u32) & ~31), 0, 32);\n\t// blr \n\treturn;'
        cases = [canonical.replace('dcbz ', 'dcbzl '), canonical.replace('dcbz ', 'dcbz128 '),
                 canonical.replace('dcbz ', 'DCBZ '), canonical.replace('r0,r11', 'r00,r11'),
                 canonical.replace('r0,r11', 'r0,r32'), canonical.replace('r0,r11', 'r0, r11'),
                 canonical.replace('~31', '~127'), canonical.replace('0, 32', '0, 128'),
                 canonical.replace('0, 32', '1, 32'), canonical.replace('ctx.r11.u32', 'ctx.r12.u32'),
                 canonical.replace('ctx.r11.u32', 'ctx.r11.u64'),
                 canonical.replace('\tmemset(base + ((ctx.r11.u32) & ~31), 0, 32);\n', ''),
                 canonical.replace('\t// blr', '\tctx.r3.u64 = 1;\n\t// blr'),
                 canonical.replace('\t// blr', 'loc_1004: ctx.r3.u64 = 1;\n\t// blr')]
        for body in cases:
            with self.subTest(body=body):
                _, reasons, _ = diagnostic.inspect_body(body, 0x1000, {'entry'}, {})
                self.assertIn('unsupported_cache_zero', reasons)

    def test_dcbz_does_not_resolve_unrelated_unsupported_events(self):
        body = '\t// dcbz r0,r11\n\tmemset(base + ((ctx.r11.u32) & ~31), 0, 32);\n\t// blr \n\treturn;'
        self.prepare([('entry', 0x1000, body)])
        self.log.write_text('Unrecognized instruction at 0x1000: dcbz\n')
        result, report = self.run_generation()
        self.assertEqual(report['counts']['retained_functions'], 0)
        self.assertEqual(report['counts']['cache_block_zeroes'], 0)
        self.assertIn('logged_unsupported_instruction', report['rejected_functions'][0]['reasons'])
        self.assertNotIn('sfr::zero_cache_block(', result)

    def test_bdzf_exact_logged_blocks_preserve_all_condition_bits_and_checkpoints(self):
        bits = [(cr, bit) for cr in range(8) for bit in ('lt', 'gt', 'eq', 'so')]
        body = ''.join(f'\t// bdzf 4*cr{cr}+{bit},0x1080\n' for cr, bit in bits)
        body += 'loc_1080:\n\t// blr \n\treturn;'
        self.prepare([('entry', 0x1000, body)])
        source = self.source / 'ppc_recomp.0.cpp'
        source.write_bytes(source.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n'))
        original = source.read_bytes()
        self.log.write_text(''.join(f'Unrecognized instruction at 0x{0x1000+i*4:X}: bdzf\n' for i in range(32)))
        result, report = self.run_generation()
        self.assertEqual(report['counts']['retained_functions'], 1)
        self.assertEqual(report['counts']['retained_bdzf'], 32)
        for cr, bit in bits:
            self.assertIn(f'if (sfr::branch_counter_zero_false(ctx.ctr.u64, ctx.cr{cr}.{bit})) goto loc_1080;', result)
        self.assertIn('loc_1080:\n\tsfr::guest_checkpoint();', result)
        self.assertEqual(source.read_bytes(), original)
        self.assertEqual(report['log']['unrecognized_events'], 32)

    def test_bdzf_requires_exact_empty_blocks_logs_and_local_targets(self):
        canonical = '\t// bdzf 4*cr6+eq,0x1008\n\t// li r3,0\n\tctx.r3.u64 = 0;\nloc_1008:\n\t// blr \n\treturn;'
        cases = [
            canonical.replace('bdzf ', 'bdzfl '), canonical.replace('cr6', 'cr8'),
            canonical.replace('+eq', '+xx'), canonical.replace(',0x', ', 0x'),
            canonical.replace('0x1008', '0x1009'), canonical.replace('0x1008', '0x9000'),
            canonical.replace('loc_1008:\n', ''), canonical.replace('loc_1008:', 'loc_1004:'),
            canonical.replace('\tctx.r3.u64 = 0;\nloc_1008:', 'loc_1008:\n\tctx.r3.u64 = 0;'),
            canonical.replace('loc_1008:', 'loc_1008: ctx.r3.u64 = 1;'),
            canonical.replace('loc_1008:', 'loc_1008: '),
            canonical.replace('loc_1008:', 'loc_1008:\t'),
            canonical.replace('0x1008\n', '0x1008\n\tctx.ctr.u64 = 0;\n'),
            canonical.replace('0x1008\n', '0x1008\nloc_1004:\n\tctx.r3.u64 = 1;\n'),
            canonical.replace('0x1008', '0x01008'), canonical.replace('bdzf', 'BDZF'),
        ]
        events = {0x1000: [('unrecognized', 'bdzf')]}
        for body in cases:
            with self.subTest(body=body):
                _, reasons, _ = diagnostic.inspect_body(body, 0x1000, {'entry'}, events)
                self.assertIn('unsupported_bdzf', reasons)
        for bad_events in ({}, {0x1000: [('unrecognized', 'bdzf'), ('decode', None)]},
                           {0x1000: [('unrecognized', 'bdzfl')]}):
            with self.subTest(events=bad_events):
                _, reasons, _ = diagnostic.inspect_body(canonical, 0x1000, {'entry'}, bad_events)
                self.assertIn('unsupported_bdzf', reasons)

    def test_bdzf_backward_target_and_unrelated_events_remain_checked(self):
        body = 'loc_1000:\n\t// bdzf 4*cr6+eq,0x1000\n\t// blr \n\treturn;'
        self.prepare([('entry', 0x1000, body)])
        self.log.write_text('Unrecognized instruction at 0x1000: bdzf\n' * 2)
        result, report = self.run_generation()
        self.assertEqual(report['counts']['retained_bdzf'], 1)
        self.assertIn('goto loc_1000;', result)
        _, reasons, details = diagnostic.inspect_body(body, 0x1000, {'entry'},
            {0x1000: [('unrecognized', 'bdzf')], 0x1004: [('unrecognized', 'unknown')]})
        self.assertIn('logged_unsupported_instruction', reasons)
        self.assertEqual(details['unsupported_addresses'], ['0x00001004'])

    def test_arithmetic_loop_labels_include_execution_safe_points(self):
        body = ('loc_1000:\n\t// addi r3,r3,1\n\tctx.r3.s64 = ctx.r3.s64 + 1;\n'
                'loc_1004:\n\t// b 0x1000\n\tgoto loc_1000;')
        self.prepare([('loop', 0x1000, body)])
        report = generate(self.source, self.log, self.output)
        result = (self.output / 'ppc_recomp.0.cpp').read_text()
        self.assertEqual(report['counts']['retained_functions'], 1)
        self.assertIn('loc_1000:\n\tsfr::guest_checkpoint();', result)
        self.assertIn('loc_1004:\n\tsfr::guest_checkpoint();', result)
        self.assertIn('ctx.r3.s64 = ctx.r3.s64 + 1;', result)
        self.assertIn('goto loc_1000;', result)

    def setUp(self):
        self.assertIsNotNone(generate, 'diagnostic generator implementation missing')
        repository = Path(__file__).resolve().parents[1]
        # Keep generated fixtures with ignored build output. Workspace-root
        # placement reproduced transient Windows rename failures in A/B probes.
        scratch = (repository / 'out/test-tmp').resolve()
        self.assertTrue(scratch.is_relative_to(repository), 'test scratch must stay inside repository')
        scratch.mkdir(parents=True, exist_ok=True)
        self.root = scratch / ('diagnostic-test-' + uuid.uuid4().hex)
        self.root.mkdir()
        self.addCleanup(shutil.rmtree, self.root)
        self.source = self.root / 'ppc'
        self.source.mkdir()
        self.output = self.root / 'diagnostic'
        self.log = self.root / 'recompile.log'
        self.log.write_text('Recompiling functions... 100%\n')

    def prepare(self, functions, imports=()):
        names = [name for name, _, _ in functions] + [name for name, _ in imports]
        (self.source / 'ppc_recomp_shared.h').write_text(
            '\n'.join(f'PPC_EXTERN_FUNC({name});' for name in names))
        for header in ('ppc_context.h', 'ppc_config.h'):
            (self.source / header).write_text('#pragma once\n')
        (self.source / 'ppc_func_mapping.cpp').write_text(
            '#include "ppc_recomp_shared.h"\nPPCFuncMapping PPCFuncMappings[] = {\n' +
            ''.join(f'\t{{ 0x{address:X}, {name} }},\n' for name, address, _ in functions) +
            ''.join(f'\t{{ 0x{address:X}, {name} }},\n' for name, address in imports) +
            '\t{ 0, nullptr }\n};\n')
        source = '#include "ppc_recomp_shared.h"\n\n' + ''.join(
            function(name, body) for name, _, body in functions)
        (self.source / 'ppc_recomp.0.cpp').write_text(source)
        return source

    def run_generation(self):
        generate(self.source, self.log, self.output)
        return (self.output / 'ppc_recomp.0.cpp').read_text(), json.loads(
            (self.output / 'report.json').read_text())

    def vector_mask(self, values=range(15, -1, -1)):
        (self.source / 'ppc_context.h').write_text(
            'inline uint8_t VectorMaskL[] = {\n' +
            ', '.join(hex(value) for value in values) + ',\n};\n')

    def test_vector_word_store_hooks_keep_effective_address_and_source(self):
        self.prepare([('wordstores', 0x1000, '\tuint32_t ea{};\n' +
            vector_word_store() + vector_word_store('stvewx', 31, 'r7', 0) +
            vector_word_store('stvewx128', 127, '0', 31) + vector_word_store('stvewx', 0, 'r31', 1) +
            '\t// blr \n\treturn;')])
        self.vector_mask()
        source = self.source / 'ppc_recomp.0.cpp'
        source.write_bytes(source.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n'))
        original = source.read_bytes()
        result, report = self.run_generation()
        self.assertEqual(report['counts']['retained_functions'], 1)
        self.assertEqual(report['counts']['vector_word_stores'], 4)
        for reg, ea in [(43, 'ctx.r11.u32'), (31, 'ctx.r7.u32 + ctx.r0.u32'),
                        (127, 'ctx.r31.u32'), (0, 'ctx.r31.u32 + ctx.r1.u32')]:
            self.assertIn(f'ea = ({ea}) & ~0x3;\n\tsfr::store_vector_word(ea, ctx.v{reg}.u8);', result)
        self.assertNotIn('PPC_STORE_U32(ea,', result)
        self.assertEqual(source.read_bytes(), original)

    def test_vector_word_store_rejects_mismatches_and_unsupported_widths(self):
        bodies = [vector_word_store('stvewx', 32), vector_word_store('stvewx128', 128),
                  vector_word_store().replace('ctx.v43.u32', 'ctx.v42.u32'),
                  vector_word_store().replace('& ~0x3', '& ~0x1'),
                  vector_word_store().replace('ctx.r11.u32', 'ctx.r10.u32'),
                  vector_word_store().replace('3 - ((ea', '2 - ((ea'),
                  vector_word_store().replace('PPC_STORE_U32', 'PPC_STORE_U64'),
                  vector_word_store().replace('>> 2', '>> 1'),
                  vector_word_store().replace('(ea, ctx.', '(ea + 4, ctx.'),
                  '\t// stvewx128 v43,r0,r11\n', '\t// stvehx v0,r0,r3\n',
                  '\t// lvewx128 v43,r0,r11\n']
        self.prepare([(f'badword{i}', 0x1000 + i * 0x10, body) for i, body in enumerate(bodies)])
        self.vector_mask()
        result, report = self.run_generation()
        self.assertEqual(report['counts']['rejected_functions'], len(bodies))
        self.assertEqual(report['counts']['vector_word_stores'], 0)
        for item in report['rejected_functions']:
            self.assertIn('unsupported_vector_memory', item['reasons'])
        self.assertNotIn('sfr::store_vector_word', result)

    def test_word_only_vector_rewrite_requires_mask(self):
        self.prepare([('wordstore', 0x1000, vector_word_store())])
        with self.assertRaisesRegex(ValueError, 'VectorMaskL'):
            generate(self.source, self.log, self.output)
        self.assertFalse(self.output.exists())
        self.vector_mask(range(16))
        with self.assertRaisesRegex(ValueError, 'VectorMaskL'):
            generate(self.source, self.log, self.output)
        self.assertFalse(self.output.exists())

    def test_full_vector_hooks_preserve_emitter_registers_and_source(self):
        self.prepare([('vectors', 0x1000,
            vector_memory() + vector_memory('stvx128', 127, 'r7', 0) +
            vector_memory('lvx', 31, '0', 31) + vector_memory('stvx', 0, 'r31', 1) +
            '\t// blr \n\treturn;')])
        self.vector_mask()
        source = self.source / 'ppc_recomp.0.cpp'
        source.write_bytes(source.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n'))
        original = source.read_bytes()
        result, report = self.run_generation()
        self.assertEqual(report['counts']['retained_functions'], 1)
        self.assertEqual(report['counts']['vector_loads'], 2)
        self.assertEqual(report['counts']['vector_stores'], 2)
        for expected in ('load_vector_memory(uint32_t(ctx.r11.u32), ctx.v63.u8)',
                         'store_vector_memory(uint32_t(ctx.r7.u32 + ctx.r0.u32), ctx.v127.u8)',
                         'load_vector_memory(uint32_t(ctx.r31.u32), ctx.v31.u8)',
                         'store_vector_memory(uint32_t(ctx.r31.u32 + ctx.r1.u32), ctx.v0.u8)'):
            self.assertIn('sfr::' + expected, result)
        self.assertNotIn('base +', result)
        self.assertEqual(source.read_bytes(), original)

    def test_unmatched_vector_memory_stops_including_noop_emissions(self):
        bodies = [vector_memory('lvewx128'), vector_memory('lvx', 32),
                  vector_memory('stvx128', 128), vector_memory().replace('ctx.v63.u8', 'ctx.v62.u8'),
                  vector_memory().replace('VectorMaskL', 'VectorMaskR'),
                  vector_memory().replace('& ~0xF', '& ~0x7'),
                  '\t// lvx128 v63,r0,r11\n', '\t// lvlx128 v1,r0,r3\n',
                  '\t// stvewx v1,r0,r3\n']
        self.prepare([(f'badvector{i}', 0x1000 + i * 0x10, body)
                      for i, body in enumerate(bodies)])
        self.vector_mask()
        result, report = self.run_generation()
        self.assertEqual(report['counts']['rejected_functions'], len(bodies))
        self.assertEqual(report['counts']['vector_loads'], 0)
        for item in report['rejected_functions']:
            self.assertIn('unsupported_vector_memory', item['reasons'])
        self.assertNotIn('base +', result)

    def test_mask_building_instructions_are_not_memory_accesses(self):
        self.prepare([('masks', 0x1000,
                       '\t// lvsl v1,r0,r3\n\t// lvsr128 v63,r0,r3\n\t// blr \n\treturn;')])
        _, report = self.run_generation()
        self.assertEqual(report['counts']['retained_functions'], 1)

    def test_vector_rewrite_requires_verified_mask_before_publication(self):
        self.prepare([('vectors', 0x1000, vector_memory())])
        for index, values in enumerate((None, range(16), range(15, 0, -1))):
            with self.subTest(values=values):
                self.output = self.root / f'diagnostic-mask-{index}'
                if values is not None:
                    self.vector_mask(values)
                with self.assertRaisesRegex(ValueError, 'VectorMaskL'):
                    generate(self.source, self.log, self.output)
                self.assertFalse(self.output.exists())

    def test_vector_mask_rejects_ambiguous_or_nonliteral_initializers(self):
        self.prepare([('vectors', 0x1000, vector_memory())])
        self.vector_mask()
        path = self.source / 'ppc_context.h'
        original = path.read_text()
        for index, header in enumerate((original + original, original.replace('0xf', '017'),
                                        original.replace('0xf', '(15)'), original.replace('0xf', '0x100'))):
            with self.subTest(index=index):
                self.output = self.root / f'diagnostic-mask-invalid-{index}'
                path.write_text(header)
                with self.assertRaisesRegex(ValueError, 'VectorMaskL'):
                    generate(self.source, self.log, self.output)
                self.assertFalse(self.output.exists())

    def test_valid_nested_body_and_alias_preserved_and_input_untouched(self):
        raw = self.prepare([('named_function', 0x1000,
            '\t// li r3,0\n\tif (ctx.r3.u32) { ctx.r3.u32 = 0; }\n'
            '\t// blr \n\treturn;')])
        result, report = self.run_generation()
        instrumentation = '\n\tsfr::enter_function(ctx, "named_function", 0x00001000);'
        self.assertIn(instrumentation, result)
        self.assertIn(raw, result.replace(instrumentation, ''))
        self.assertEqual((self.source / 'ppc_recomp.0.cpp').read_text(), raw)
        self.assertEqual(report['counts']['rejected_functions'], 0)

    def test_logged_bad_address_rejects_both_overlapping_functions(self):
        self.prepare([('outer', 0x1000, '\t// li r3,0\n\tctx.r3.u32 = 0;\n'
                       '\t// subfze r4,r3\n\tctx.r4.u64 = 0;\n\t// blr \n\treturn;'),
                      ('inner', 0x1004, '\t// subfze r4,r3\n\tctx.r4.u64 = 0;\n'
                       '\t// blr \n\treturn;')])
        self.log.write_text('Unrecognized instruction at 0x1004: subfze\n' * 2)
        result, report = self.run_generation()
        self.assertEqual(result.count('sfr::unsupported_function('), 2)
        self.assertIn('"outer", 0x00001000', result)
        self.assertEqual(report['log']['unrecognized_events'], 2)
        self.assertEqual(report['counts']['rejected_functions'], 2)

    def test_logged_plain_subfze_empty_block_is_retained_and_rewritten(self):
        self.prepare([('entry', 0x1000,
                       '\t// li r3,0\n\tctx.r3.u64 = 0;\n'
                       '\t// subfze r31,r0\n\n\t// blr \n\treturn;')])
        self.log.write_text('Unrecognized instruction at 0x1004: subfze\n' * 2)
        result, report = self.run_generation()
        self.assertIn('\t// subfze r31,r0\n\tsfr::subfze(ctx.r31.u64, ctx.r0.u64, ctx.xer.ca);', result)
        self.assertEqual(report['counts']['retained_subfze'], 1)
        self.assertEqual(report['counts']['retained_functions'], 1)
        self.assertEqual(report['log']['unrecognized_events'], 2)

    def test_subfze_accepts_all_registers_aliasing_and_crlf(self):
        body = ''.join(f'\t// subfze r{register},r{register}\n'
                       for register in range(32)) + '\t// blr \n\treturn;'
        self.prepare([('entry', 0x1000, body)])
        source = self.source / 'ppc_recomp.0.cpp'
        source.write_bytes(source.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n'))
        self.log.write_text(''.join(
            f'Unrecognized instruction at 0x{0x1000 + register * 4:X}: subfze\r\n'
            for register in range(32)))
        _, report = self.run_generation()
        generated = (self.output / 'ppc_recomp.0.cpp').read_bytes()
        for register in range(32):
            self.assertIn(f'sfr::subfze(ctx.r{register}.u64, ctx.r{register}.u64, ctx.xer.ca);\r\n'.encode(),
                          generated)
        self.assertEqual(report['counts']['retained_subfze'], 32)

    def test_subfze_requires_only_matching_log_events_at_address(self):
        logs = [
            'Unrecognized instruction at 0x1000: sfze\n',
            ('Unrecognized instruction at 0x1000: subfze\n'
             'Unable to decode instruction DEADBEEF at 1000\n'),
            ('Unrecognized instruction at 0x1000: subfze\n'
             'subfze at 1000 has RC bit enabled but no comparison was generated\n'),
        ]
        for index, log in enumerate(logs):
            with self.subTest(index=index):
                self.output = self.root / f'diagnostic-log-mismatch-{index}'
                self.prepare([('entry', 0x1000, '\t// subfze r4,r3\n\t// blr \n\treturn;')])
                self.log.write_text(log)
                result, report = self.run_generation()
                self.assertIn('sfr::unsupported_function', result)
                self.assertNotIn('sfr::subfze(', result)
                self.assertIn('logged_unsupported_instruction', report['rejected_functions'][0]['reasons'])

    def test_subfze_without_log_evidence_is_rejected(self):
        self.prepare([('entry', 0x1000, '\t// subfze r4,r3\n\t// blr \n\treturn;')])
        result, report = self.run_generation()
        self.assertIn('sfr::unsupported_function', result)
        self.assertNotIn('sfr::subfze(', result)
        self.assertIn('unsupported_subfze', report['rejected_functions'][0]['reasons'])

    def test_subfze_like_spellings_without_log_evidence_are_rejected(self):
        bodies = ['\t//  subfze r4,r3', '\t// SUBFZE r4,r3',
                  '\t// subfzex r4,r3', '\t// SFZEO. r4,r3']
        self.prepare([(f'badspelling{i}', 0x1000 + i * 0x10, body)
                      for i, body in enumerate(bodies)])
        result, report = self.run_generation()
        self.assertNotIn('sfr::subfze(', result)
        self.assertEqual(result.count('sfr::unsupported_function('), len(bodies))
        self.assertEqual(report['counts']['rejection_reasons']['unsupported_subfze'], len(bodies))

    def test_subfze_with_emitted_code_is_rejected(self):
        self.prepare([('entry', 0x1000,
                       '\t// subfze r4,r3\n\tctx.r4.u64 = ~ctx.r3.u64;\n'
                       '\t// blr \n\treturn;')])
        self.log.write_text('Unrecognized instruction at 0x1000: subfze\n')
        result, report = self.run_generation()
        self.assertIn('sfr::unsupported_function', result)
        self.assertNotIn('sfr::subfze(', result)
        self.assertIn('unsupported_subfze', report['rejected_functions'][0]['reasons'])

    def test_subfze_variants_and_malformed_registers_are_rejected(self):
        bodies = ['\t// subfze. r4,r3', '\t// subfzeo r4,r3', '\t// subfzeo. r4,r3',
                  '\t// sfze r4,r3', '\t// subfze r32,r3', '\t// subfze r4,r32',
                  '\t// subfze r04,r3', '\t// subfze r4, r3']
        self.prepare([(f'badsubfze{i}', 0x1000 + i * 0x10, body)
                      for i, body in enumerate(bodies)])
        self.log.write_text(''.join(
            f'Unrecognized instruction at 0x{0x1000 + i * 0x10:X}: subfze\n'
            for i in range(len(bodies))))
        result, report = self.run_generation()
        self.assertNotIn('sfr::subfze(', result)
        self.assertEqual(report['counts']['rejected_functions'], len(bodies))
        self.assertTrue(all('unsupported_subfze' in item['reasons'] or
                            'logged_unsupported_instruction' in item['reasons']
                            for item in report['rejected_functions']))

    def test_overlapping_functions_validate_subfze_independently(self):
        self.prepare([('outer', 0x1000, '\t// li r3,0\n\tctx.r3.u32 = 0;\n'
                       '\t// subfze r4,r3\n\t// blr \n\treturn;'),
                      ('inner', 0x1004, '\t// subfze r4,r3\n\tctx.r4.u64 = 0;\n'
                       '\t// blr \n\treturn;')])
        self.log.write_text('Unrecognized instruction at 0x1004: subfze\n')
        result, report = self.run_generation()
        self.assertEqual(result.count('sfr::subfze('), 1)
        self.assertEqual(result.count('sfr::unsupported_function('), 1)
        self.assertEqual(report['counts']['retained_subfze'], 1)
        self.assertEqual(report['counts']['rejected_functions'], 1)

    def test_other_unsupported_event_elsewhere_remains_rejected(self):
        self.prepare([('entry', 0x1000,
                       '\t// subfze r4,r3\n\t// nop \n\t// blr \n\treturn;')])
        self.log.write_text('Unrecognized instruction at 0x1000: subfze\n'
                            'Unable to decode instruction DEADBEEF at 1004\n')
        result, report = self.run_generation()
        self.assertIn('sfr::unsupported_function', result)
        self.assertNotIn('sfr::subfze(', result)
        self.assertEqual(report['counts']['retained_subfze'], 0)
        self.assertIn('logged_unsupported_instruction', report['rejected_functions'][0]['reasons'])

    def test_logged_sthu_real_pattern_is_retained_with_instruction_addresses(self):
        body = '\t// sthu r8,2(r9)\n\nloc_825E4958:\n\t// blr \n\treturn;'
        self.prepare([('entry', 0x825E4954, body)])
        original = (self.source / 'ppc_recomp.0.cpp').read_bytes()
        self.log.write_text('Unrecognized instruction at 0x825E4954: sthu\n' * 2)
        result, report = self.run_generation()
        self.assertIn('// sthu r8,2(r9)\n\tsfr::store_halfword_update('
                      '*sfr::active_memory, ctx.r9.u64, ctx.r8.u64, 2);', result)
        self.assertIn('loc_825E4958:', result)
        self.assertEqual(report['counts']['retained_sthu'], 1)
        self.assertEqual(report['counts']['retained_functions'], 1)
        self.assertEqual((self.source / 'ppc_recomp.0.cpp').read_bytes(), original)
        end, reasons, details = diagnostic.inspect_body(
            '\n' + body, 0x825E4954, {'entry'},
            {0x825E4954: [('unrecognized', 'sthu')]})
        self.assertEqual(end, 0x825E495C)
        self.assertEqual(reasons, [])
        self.assertEqual(details['retained_sthu_addresses'], ['0x825E4954'])

    def test_sthu_all_registers_aliasing_displacement_limits_and_crlf(self):
        operands = [(register, max(register, 1), displacement)
                    for register in range(32) for displacement in (-32768, -1, 0, 32767)]
        body = ''.join(f'\t// sthu r{source},{displacement}(r{base})\n'
                       for source, base, displacement in operands)
        self.prepare([('entry', 0x1000, body + '\t// blr \n\treturn;')])
        path = self.source / 'ppc_recomp.0.cpp'
        path.write_bytes(path.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n'))
        self.log.write_text(''.join(f'Unrecognized instruction at 0x{0x1000 + index * 4:X}: sthu\n'
                                    for index in range(len(operands))))
        _, report = self.run_generation()
        generated = (self.output / 'ppc_recomp.0.cpp').read_bytes()
        for source, base, displacement in operands:
            self.assertIn((f'\tsfr::store_halfword_update(*sfr::active_memory, ctx.r{base}.u64, '
                           f'ctx.r{source}.u64, {displacement});\r\n').encode(), generated)
        self.assertEqual(report['counts']['retained_sthu'], len(operands))

    def test_sthu_rejects_noncanonical_operands_and_related_opcodes(self):
        forms = ['sthu r8,2(r0)', 'sthu r32,2(r9)', 'sthu r8,2(r32)',
                 'sthu r08,2(r9)', 'sthu r8,2(r09)', 'sthu r8,32768(r9)',
                 'sthu r8,-32769(r9)', 'sthu r8,+2(r9)', 'sthu r8,02(r9)',
                 'sthu r8,-0(r9)', 'sthu r8,0x2(r9)', 'sthu r8,-02(r9)',
                 'sthu r8, 2(r9)', 'sthu r8,2 (r9)', 'sthu r8,2(r9) ',
                 'sthu  r8,2(r9)', ' sthu r8,2(r9)', 'STHU r8,2(r9)',
                 'sthux r8,r9,r10', 'sthu. r8,2(r9)', 'sthu r8,2(r9',
                 'sthu r8,2(0)', 'sthu r8,2(r-1)', 'sthu r8,2(r9) extra']
        self.prepare([(f'bad{i}', 0x1000 + i * 0x10, '\t// ' + form)
                      for i, form in enumerate(forms)])
        self.log.write_text(''.join(f'Unrecognized instruction at 0x{0x1000 + i * 0x10:X}: sthu\n'
                                    for i in range(len(forms))))
        result, report = self.run_generation()
        self.assertNotIn('sfr::store_halfword_update(', result)
        self.assertEqual(report['counts']['rejection_reasons']['unsupported_sthu'], len(forms))
        self.assertEqual(report['counts']['retained_sthu'], 0)

    def test_sthu_unlogged_plain_and_related_forms_fail_closed(self):
        # sthux is an indexed form handled with the supplemental instructions.
        forms = ['sthu r8,2(r9)', 'sthu. r8,2(r9)', ' STHU r8,2(r9)']
        self.prepare([(f'unlogged{i}', 0x1000 + i * 0x10, '\t// ' + form)
                      for i, form in enumerate(forms)])
        result, report = self.run_generation()
        self.assertNotIn('sfr::store_halfword_update(', result)
        self.assertEqual(report['counts']['rejection_reasons']['unsupported_sthu'], len(forms))

    def test_sthu_requires_all_log_events_to_match(self):
        logs = ['Unrecognized instruction at 0x1000: sthux\n',
                'Unrecognized instruction at 0x1000: sthu\nUnable to decode instruction B5090002 at 1000\n',
                'Unrecognized instruction at 0x1000: sthu\nUnrecognized instruction at 0x1000: unknown\n',
                'Unrecognized instruction at 0x1000: sthu\nsthu at 1000 has RC bit enabled but no comparison was generated\n',
                'Unrecognized instruction at 0x1000: sthu\nFound a switch jump table at 1000 with no switch table entry present\n']
        for index, log in enumerate(logs):
            with self.subTest(index=index):
                self.output = self.root / f'bad-log-{index}'
                self.prepare([('entry', 0x1000, '\t// sthu r8,2(r9)')])
                self.log.write_text(log)
                result, report = self.run_generation()
                self.assertNotIn('sfr::store_halfword_update(', result)
                self.assertIn('unsupported_sthu', report['rejected_functions'][0]['reasons'])
                self.assertIn('logged_unsupported_instruction', report['rejected_functions'][0]['reasons'])

    def test_sthu_log_cannot_resolve_a_different_instruction(self):
        self.prepare([('entry', 0x1000, '\t// nop ')])
        self.log.write_text('Unrecognized instruction at 0x1000: sthu\n')
        _, report = self.run_generation()
        self.assertIn('unsupported_sthu', report['rejected_functions'][0]['reasons'])
        self.assertIn('logged_unsupported_instruction', report['rejected_functions'][0]['reasons'])

    def test_sthu_existing_emission_or_error_is_rejected(self):
        emissions = ['\tctx.r9.u64 += 2;\n', '\tPPC_STORE_U16(ctx.r9.u32, ctx.r8.u16);\n',
                     '\t// ERROR unknown emission\n', '\t/* partial emission */\n']
        self.prepare([(f'partial{i}', 0x1000 + i * 0x10,
                       '\t// sthu r8,2(r9)\n' + emission + '\t// blr \n\treturn;')
                      for i, emission in enumerate(emissions)])
        self.log.write_text(''.join(f'Unrecognized instruction at 0x{0x1000 + i * 0x10:X}: sthu\n'
                                    for i in range(len(emissions))))
        result, report = self.run_generation()
        self.assertNotIn('sfr::store_halfword_update(', result)
        self.assertEqual(report['counts']['rejection_reasons']['unsupported_sthu'], len(emissions))

    def test_overlapping_functions_validate_sthu_independently(self):
        self.prepare([('outer', 0x1000, '\t// nop \n\t// sthu r8,2(r9)\n\t// blr \n\treturn;'),
                      ('inner', 0x1004, '\t// sthu r8,2(r9)\n\tctx.r9.u64 += 2;\n\t// blr \n\treturn;')])
        self.log.write_text('Unrecognized instruction at 0x1004: sthu\n')
        result, report = self.run_generation()
        self.assertEqual(result.count('sfr::store_halfword_update('), 1)
        self.assertEqual(report['counts']['retained_sthu'], 1)
        self.assertEqual(report['rejected_functions'][0]['name'], 'inner')
        self.assertEqual(report['rejected_functions'][0]['unsupported_addresses'], ['0x00001004'])

    def test_sthu_and_subfze_resolve_together_but_other_hazards_remain(self):
        body = '\t// sthu r8,2(r9)\n\t// subfze r4,r3\n\t// blr \n\treturn;'
        self.prepare([('entry', 0x1000, body)])
        self.log.write_text('Unrecognized instruction at 0x1000: sthu\n'
                            'Unrecognized instruction at 0x1004: subfze\n')
        result, report = self.run_generation()
        self.assertIn('sfr::store_halfword_update(', result)
        self.assertIn('sfr::subfze(', result)
        self.assertEqual(report['counts']['retained_functions'], 1)
        self.output = self.root / 'other-hazard'
        self.log.write_text(self.log.read_text() + 'Unrecognized instruction at 0x1008: unknown\n')
        result, report = self.run_generation()
        self.assertNotIn('sfr::store_halfword_update(', result)
        self.assertNotIn('sfr::subfze(', result)
        self.assertEqual(report['counts']['retained_sthu'], 0)
        self.assertEqual(report['rejected_functions'][0]['unsupported_addresses'], ['0x00001008'])
        self.assertEqual(report['rejected_functions'][0]['retained_sthu_addresses'], ['0x00001000'])

    def test_sthu_rewrite_does_not_bypass_other_rejection_reasons(self):
        cases = [('\t__builtin_debugtrap();', 'debugtrap'),
                 ('\tgoto loc_9999;', 'undefined_label'),
                 ('\tctx.r3.u32 = *(base + 12);', 'unchecked_guest_memory')]
        self.prepare([(f'hazard{i}', 0x1000 + i * 0x10,
                       '\t// sthu r8,2(r9)\n\t// nop \n' + code)
                      for i, (code, _) in enumerate(cases)])
        self.log.write_text(''.join(f'Unrecognized instruction at 0x{0x1000 + i * 0x10:X}: sthu\n'
                                    for i in range(len(cases))))
        result, report = self.run_generation()
        self.assertNotIn('sfr::store_halfword_update(', result)
        self.assertEqual(report['counts']['retained_sthu'], 0)
        for item, (_, reason) in zip(report['rejected_functions'], cases):
            self.assertIn(reason, item['reasons'])

    def test_eqv_real_pc_resolves_duplicate_logs_once_and_preserves_source(self):
        body = ('\t// nop \n\t__nop();\n\t// eqv r7,r4,r11\n'
                'loc_8222B538:\n\t// blr \n\treturn;')
        self.prepare([('entry', 0x8222B530, body)])
        source = self.source / 'ppc_recomp.0.cpp'
        before = source.read_bytes()
        self.log.write_text('Unrecognized instruction at 0x8222B534: eqv\n' * 2)
        expected = 'ctx.r7.u64 = ~(ctx.r4.u64 ^ ctx.r11.u64);'
        result, report = self.run_generation()
        self.assertEqual(result.count(expected), 1)
        self.assertIn('\t__nop();', result)
        self.assertIn('loc_8222B538:\n\tsfr::guest_checkpoint();', result)
        self.assertEqual(source.read_bytes(), before)
        self.assertEqual(report['counts']['retained_eqv'], 1)
        self.assertEqual(report['counts']['retained_functions'], 1)
        rewritten, resolved, invalid = diagnostic.rewrite_eqv(body, 0x8222B530,
            {0x8222B534: [('unrecognized', 'eqv'), ('unrecognized', 'eqv')]})
        self.assertEqual(resolved, {0x8222B534})
        self.assertEqual(invalid, [])
        self.assertEqual(rewritten, body.replace('\t// eqv r7,r4,r11\n',
                         '\t// eqv r7,r4,r11\n\t' + expected + '\n'))
        end, reasons, details = diagnostic.inspect_body(body, 0x8222B530, {'entry'},
            {0x8222B534: [('unrecognized', 'eqv')]})
        self.assertEqual(end, 0x8222B53C)
        self.assertEqual(reasons, [])
        self.assertEqual(details['retained_eqv_addresses'], ['0x8222B534'])

    def test_eqv_all_register_positions_aliases_and_crlf_use_full_u64(self):
        operands = [(r, (r + 1) % 32, (r + 7) % 32) for r in range(32)]
        for r in range(32):
            operands.extend([(r, r, 31 - r), (r, 31 - r, r),
                             (r, 31 - r, 31 - r), (r, r, r)])
        body = ''.join(f'\t// eqv r{d},r{s},r{b}\n' for d, s, b in operands)
        body += f'loc_{0x1000 + len(operands) * 4:08X}:\n\t// blr \n\treturn;'
        self.prepare([('entry', 0x1000, body)])
        source = self.source / 'ppc_recomp.0.cpp'
        source.write_bytes(source.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n'))
        before = source.read_bytes()
        self.log.write_text(''.join(f'Unrecognized instruction at 0x{0x1000 + i * 4:X}: eqv\n'
                                   for i in range(len(operands))))
        _, report = self.run_generation()
        generated = (self.output / 'ppc_recomp.0.cpp').read_bytes()
        for d, s, b in operands:
            self.assertIn((f'\t// eqv r{d},r{s},r{b}\r\n'
                           f'\tctx.r{d}.u64 = ~(ctx.r{s}.u64 ^ ctx.r{b}.u64);\r\n').encode(), generated)
        self.assertNotIn(b'ctx.cr0', generated)
        self.assertNotIn(b'ctx.xer', generated)
        self.assertEqual(source.read_bytes(), before)
        self.assertEqual(report['counts']['retained_eqv'], len(operands))
        _, resolved, invalid = diagnostic.rewrite_eqv(body, 0x1000,
            {0x1000 + i * 4: [('unrecognized', 'eqv')] for i in range(len(operands))})
        self.assertEqual(resolved, {0x1000 + i * 4 for i in range(len(operands))})
        self.assertEqual(invalid, [])

    def test_eqv_rejects_rc_malformed_registers_spelling_and_unlogged_forms(self):
        forms = ['eqv. r7,r4,r11', 'eqvo r7,r4,r11', 'EQV r7,r4,r11',
                 'eqv r32,r4,r11', 'eqv r7,r32,r11', 'eqv r7,r4,r32',
                 'eqv r00,r4,r11', 'eqv r7,r04,r11', 'eqv r7,r4,r011',
                 'eqv r-1,r4,r11', 'eqv r7,r4,f11', 'eqv r7,r4,11',
                 'eqv r7, r4,r11', 'eqv r7,r4, r11', 'eqv r7,r4,r11 ',
                 ' eqv r7,r4,r11', 'eqv r7,r4,r11 extra', 'eqv r7,r4',
                 'eqv r7,r4,r11,r0', 'eqv r7,r4,r+1']
        self.prepare([(f'bad{i}', 0x1000 + i * 0x10, '\t// ' + form)
                      for i, form in enumerate(forms)] + [('unlogged', 0x2000, '\t// eqv r7,r4,r11')])
        self.log.write_text(''.join(f'Unrecognized instruction at 0x{0x1000 + i * 0x10:X}: eqv\n'
                                   for i in range(len(forms))))
        result, report = self.run_generation()
        self.assertNotIn(' = ~(', result)
        self.assertEqual(report['counts']['retained_eqv'], 0)
        self.assertEqual(report['counts']['rejection_reasons']['unsupported_eqv'], len(forms) + 1)

    def test_eqv_requires_empty_block_including_canonical_label_gaps(self):
        gaps = ['\tctx.r7.u64 = 0;\n', '\t/* partial emission */\n',
                '\t// ERROR partial emission\n',
                'loc_00001004:\n\tctx.r7.u64 = 0;\n',
                'loc_00001004: ctx.r7.u64 = 0;\n',
                'loc_00001004: \n', 'loc_00001004:\t\n', 'custom_label:\n',
                'loc_00001004:\n\t/* hidden emission */\n']
        for index, gap in enumerate(gaps):
            self.output = self.root / f'eqv-gap-{index}'
            self.prepare([('entry', 0x1000, '\t// eqv r7,r4,r11\n' + gap + '\t// blr \n\treturn;')])
            self.log.write_text('Unrecognized instruction at 0x1000: eqv\n')
            with self.subTest(index=index):
                result, report = self.run_generation()
                self.assertNotIn(' = ~(', result)
                self.assertEqual(report['counts']['retained_eqv'], 0)
                self.assertIn('unsupported_eqv', report['rejected_functions'][0]['reasons'])
        self.output = self.root / 'eqv-wrong-label-pc'
        self.prepare([('entry', 0x1000, '\t// eqv r7,r4,r11\nloc_00001008:\n\t// blr \n\treturn;')])
        result, report = self.run_generation()
        self.assertNotIn(' = ~(', result)
        self.assertIn('uncertain_instruction_range', report['rejected_functions'][0]['reasons'])

    def test_eqv_logs_are_exact_and_cannot_resolve_a_different_pc(self):
        events = ['Unrecognized instruction at 0x1000: eqv.\n',
                  'Unrecognized instruction at 0x1000: other\n',
                  'Unable to decode instruction 7C872A38 at 1000\n',
                  'eqv at 1000 has RC bit enabled but no comparison was generated\n',
                  'Found a switch jump table at 1000 with no switch table entry present\n']
        for index, extra in enumerate(events):
            self.output = self.root / f'eqv-log-{index}'
            self.prepare([('entry', 0x1000, '\t// eqv r7,r4,r11\n\t// blr \n\treturn;')])
            self.log.write_text('Unrecognized instruction at 0x1000: eqv\n' + extra)
            with self.subTest(index=index):
                result, report = self.run_generation()
                self.assertNotIn(' = ~(', result)
                self.assertEqual(report['counts']['retained_eqv'], 0)
                self.assertIn('unsupported_eqv', report['rejected_functions'][0]['reasons'])
                self.assertIn('logged_unsupported_instruction', report['rejected_functions'][0]['reasons'])
        self.output = self.root / 'eqv-wrong-log-pc'
        self.prepare([('entry', 0x1000, '\t// eqv r7,r4,r11\n\t// nop \n\t// blr \n\treturn;')])
        self.log.write_text('Unrecognized instruction at 0x1004: eqv\n')
        result, report = self.run_generation()
        self.assertNotIn(' = ~(', result)
        item = report['rejected_functions'][0]
        self.assertEqual(item['unsupported_addresses'], ['0x00001004'])
        self.assertEqual(item['invalid_eqv_addresses'], ['0x00001000', '0x00001004'])

    def test_eqv_overlapping_functions_and_unrelated_hazards_remain_independent(self):
        self.prepare([('outer', 0x1000, '\t// stfsu f0,12(r31)\n\t// eqv r7,r4,r11\n\t// blr \n\treturn;'),
                      ('inner', 0x1004, '\t// eqv r7,r4,r11\n\tctx.r7.u64 = 0;\n\t// blr \n\treturn;')])
        self.log.write_text('Unrecognized instruction at 0x1000: stfsu\nUnrecognized instruction at 0x1004: eqv\n')
        result, report = self.run_generation()
        self.assertEqual(result.count('ctx.r7.u64 = ~(ctx.r4.u64 ^ ctx.r11.u64);'), 1)
        self.assertEqual(report['counts']['retained_eqv'], 1)
        self.assertEqual(report['counts']['retained_stfsu'], 1)
        self.assertEqual(report['rejected_functions'][0]['name'], 'inner')
        self.output = self.root / 'eqv-other-log'
        self.log.write_text(self.log.read_text() + 'Unrecognized instruction at 0x1008: unknown\n')
        result, report = self.run_generation()
        self.assertNotIn(' = ~(', result)
        self.assertEqual(report['counts']['retained_eqv'], 0)
        self.assertEqual(report['counts']['retained_stfsu'], 0)
        cases = [('\t__builtin_debugtrap();', 'debugtrap'),
                 ('\t// ERROR unsafe', 'error_comment'),
                 ('\tctx.r3.u32 = *(uint32_t*)(base + 4);', 'unchecked_guest_memory'),
                 ('\tgoto loc_00002000;', 'undefined_label')]
        self.output = self.root / 'eqv-other-body-hazards'
        self.prepare([(f'hazard{i}', 0x1000 + i * 0x10,
                       '\t// eqv r7,r4,r11\n\t// nop \n' + code)
                      for i, (code, _) in enumerate(cases)])
        self.log.write_text(''.join(f'Unrecognized instruction at 0x{0x1000 + i * 0x10:X}: eqv\n'
                                   for i in range(len(cases))))
        result, report = self.run_generation()
        self.assertNotIn(' = ~(', result)
        self.assertEqual(report['counts']['retained_eqv'], 0)
        for item, (_, reason) in zip(report['rejected_functions'], cases):
            self.assertIn(reason, item['reasons'])
            self.assertEqual(item['retained_eqv'], 1)

    def test_stfsu_real_instruction_preserves_original_code_and_raw_fpr_bits(self):
        body = '\t// fmuls f0,f13,f4\n\tctx.f0.f64 = double(float(ctx.f13.f64 * ctx.f4.f64));\n'
        body += '\t// stfsu f0,12(r31)\nloc_8281118C:\n\t// blr \n\treturn;'
        self.prepare([('entry', 0x82811184, body)])
        original = (self.source / 'ppc_recomp.0.cpp').read_bytes()
        self.log.write_text('Unrecognized instruction at 0x82811188: stfsu\n' * 2)
        result, report = self.run_generation()
        self.assertIn('sfr::store_float_single_update(*sfr::active_memory, ctx.r31.u64, ctx.f0.u64, 12);', result)
        self.assertIn('ctx.f0.f64 = double(float(ctx.f13.f64 * ctx.f4.f64));', result)
        self.assertIn('loc_8281118C:', result)
        self.assertEqual(report['counts']['retained_stfsu'], 1)
        self.assertEqual(report['counts']['retained_functions'], 1)
        self.assertEqual((self.source / 'ppc_recomp.0.cpp').read_bytes(), original)
        end, reasons, details = diagnostic.inspect_body('\n' + body, 0x82811184, {'entry'},
            {0x82811188: [('unrecognized', 'stfsu')]})
        self.assertEqual(end, 0x82811190)
        self.assertEqual(reasons, [])
        self.assertEqual(details['retained_stfsu_addresses'], ['0x82811188'])

    def test_stfsu_registers_signed_limits_and_crlf(self):
        operands = [(source, max(source, 1), displacement) for source in range(32)
                    for displacement in (-32768, -1, 0, 32767)]
        self.prepare([('entry', 0x1000, ''.join(
            f'\t// stfsu f{source},{offset}(r{base})\n' for source, base, offset in operands)
            + '\t// blr \n\treturn;')])
        path = self.source / 'ppc_recomp.0.cpp'
        path.write_bytes(path.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n'))
        self.log.write_text(''.join(f'Unrecognized instruction at 0x{0x1000 + index * 4:X}: stfsu\n'
                                   for index in range(len(operands))))
        _, report = self.run_generation()
        generated = (self.output / 'ppc_recomp.0.cpp').read_bytes()
        for source, base, offset in operands:
            self.assertIn((f'\tsfr::store_float_single_update(*sfr::active_memory, ctx.r{base}.u64, '
                           f'ctx.f{source}.u64, {offset});\r\n').encode(), generated)
        self.assertEqual(report['counts']['retained_stfsu'], len(operands))

    def test_stfsu_rejects_unlogged_malformed_and_related_instructions(self):
        forms = ['stfsu f0,12(r0)', 'stfsu f32,12(r31)', 'stfsu f0,12(r32)',
                 'stfsu f00,12(r31)', 'stfsu f0,12(r01)', 'stfsu f0,32768(r31)',
                 'stfsu f0,-32769(r31)', 'stfsu f0,+12(r31)', 'stfsu f0,012(r31)',
                 'stfsu f0,-0(r31)', 'stfsu f0,0xC(r31)', 'stfsu f0,-012(r31)',
                 'stfsu r0,12(r31)', 'stfsu f0,12(f31)', 'stfsu f0, 12(r31)',
                 'stfsu f0,12(r31) ', ' stfsu f0,12(r31)', 'STFSU f0,12(r31)',
                 'stfsux f0,r31,r4', 'stfsu. f0,12(r31)', 'stfsu f0,12(r31) extra']
        self.prepare([(f'bad{i}', 0x1000 + i * 0x10, '\t// ' + form)
                      for i, form in enumerate(forms)] + [('unlogged', 0x2000, '\t// stfsu f0,12(r31)')])
        self.log.write_text(''.join(f'Unrecognized instruction at 0x{0x1000 + i * 0x10:X}: stfsu\n'
                                   for i in range(len(forms))))
        result, report = self.run_generation()
        self.assertNotIn('sfr::store_float_single_update(', result)
        self.assertEqual(report['counts']['retained_stfsu'], 0)
        self.assertEqual(report['counts']['rejection_reasons']['unsupported_stfsu'], len(forms) + 1)

    def test_stfsu_requires_empty_emission_and_matching_logs(self):
        cases = [('stfsu f0,12(r31)', '\tctx.r31.u64 += 12;', 'stfsu'),
                 ('stfsu f0,12(r31)', '\t// ERROR partial emission', 'stfsu'),
                 ('stfsu f0,12(r31)', '\t/* partial emission */', 'stfsu'),
                 ('stfsu f0,12(r31)', '', 'stfsux'), ('nop ', '', 'stfsu')]
        for index, (instruction, emission, opcode) in enumerate(cases):
            with self.subTest(index=index):
                self.output = self.root / f'stfsu-invalid-{index}'
                self.prepare([('entry', 0x1000, '\t// ' + instruction + '\n' + emission + '\n\t// blr \n\treturn;')])
                self.log.write_text(f'Unrecognized instruction at 0x1000: {opcode}\n')
                result, report = self.run_generation()
                self.assertNotIn('sfr::store_float_single_update(', result)
                self.assertIn('unsupported_stfsu', report['rejected_functions'][0]['reasons'])
        for index, extra in enumerate(['Unrecognized instruction at 0x1000: other\n',
                'Unable to decode instruction D41F000C at 1000\n',
                'stfsu at 1000 has RC bit enabled but no comparison was generated\n',
                'Found a switch jump table at 1000 with no switch table entry present\n']):
            self.output = self.root / f'stfsu-log-{index}'
            self.prepare([('entry', 0x1000, '\t// stfsu f0,12(r31)')])
            self.log.write_text('Unrecognized instruction at 0x1000: stfsu\n' + extra)
            _, report = self.run_generation()
            self.assertIn('unsupported_stfsu', report['rejected_functions'][0]['reasons'])
            self.assertIn('logged_unsupported_instruction', report['rejected_functions'][0]['reasons'])

    def test_stfsu_labels_cannot_hide_emitted_code(self):
        gaps = ['loc_00001004:\n\tctx.r31.u64 += 12;\n',
                'loc_00001004: ctx.r31.u64 += 12;\n', 'loc_00001004: \n',
                'custom_label:\n', 'loc_00001004:\n\t/* partial */\n']
        for index, gap in enumerate(gaps):
            self.output = self.root / f'stfsu-label-{index}'
            self.prepare([('entry', 0x1000, '\t// stfsu f0,12(r31)\n' + gap + '\t// blr \n\treturn;')])
            self.log.write_text('Unrecognized instruction at 0x1000: stfsu\n')
            result, report = self.run_generation()
            self.assertNotIn('sfr::store_float_single_update(', result)
            self.assertIn('unsupported_stfsu', report['rejected_functions'][0]['reasons'])

    def test_stfsu_overlaps_are_validated_separately_and_other_hazards_remain(self):
        self.prepare([('outer', 0x1000, '\t// sthu r8,2(r9)\n\t// stfsu f0,12(r31)\n\t// blr \n\treturn;'),
                      ('inner', 0x1004, '\t// stfsu f0,12(r31)\n\tctx.r31.u64 += 12;\n\t// blr \n\treturn;')])
        self.log.write_text('Unrecognized instruction at 0x1000: sthu\nUnrecognized instruction at 0x1004: stfsu\n')
        result, report = self.run_generation()
        self.assertEqual(result.count('sfr::store_float_single_update('), 1)
        self.assertEqual(report['counts']['retained_stfsu'], 1)
        self.assertEqual(report['counts']['retained_sthu'], 1)
        self.assertEqual(report['rejected_functions'][0]['name'], 'inner')
        self.output = self.root / 'stfsu-other-hazard'
        self.log.write_text(self.log.read_text() + 'Unrecognized instruction at 0x1008: unknown\n')
        result, report = self.run_generation()
        self.assertNotIn('sfr::store_float_single_update(', result)
        self.assertEqual(report['counts']['retained_stfsu'], 0)

    def test_logged_lhzu_canonical_operands_preserve_crlf_label_and_source(self):
        operands = [(0, 1, -32768), (31, 30, -1), (0, 31, 0), (30, 31, 32767)]
        body = ''.join(f'\t// lhzu r{target},{displacement}(r{base})\n'
                       for target, base, displacement in operands)
        body += 'loc_00001010:\n\t// blr \n\treturn;'
        self.prepare([('entry', 0x1000, body)])
        source = self.source / 'ppc_recomp.0.cpp'
        source.write_bytes(source.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n'))
        original = source.read_bytes()
        logs = ''.join(f'Unrecognized instruction at 0x{0x1000 + index * 4:X}: lhzu\n'
                       for index in range(len(operands)))
        self.log.write_text(logs + 'Unrecognized instruction at 0x1000: lhzu\n')
        _, report = self.run_generation()
        generated = (self.output / 'ppc_recomp.0.cpp').read_bytes()
        for target, base, displacement in operands:
            self.assertIn(f'\t// lhzu r{target},{displacement}(r{base})\r\n'.encode(), generated)
            hook = (f'\tsfr::load_halfword_update(*sfr::active_memory, ctx.r{target}.u64, '
                    f'ctx.r{base}.u64, {displacement});\r\n').encode()
            self.assertIn(hook, generated)
        self.assertIn(b'loc_00001010:\r\n', generated)
        self.assertEqual(source.read_bytes(), original)
        self.assertEqual(report['counts']['retained_lhzu'], len(operands))
        self.assertEqual(report['counts']['retained_functions'], 1)

    def test_lhzu_rejects_invalid_operands_related_forms_and_unlogged_code(self):
        forms = ['lhzu r8,2(r0)', 'lhzu r8,2(r8)', 'lhzu r32,2(r9)',
                 'lhzu r08,2(r9)', 'lhzu r8,2(r32)', 'lhzu r8,2(r09)',
                 'lhzu r8,32768(r9)', 'lhzu r8,-32769(r9)', 'lhzu r8,+2(r9)',
                 'lhzu r8,02(r9)', 'lhzu r8,-0(r9)', 'lhzu r8,0x2(r9)',
                 'lhzu r8, 2(r9)', 'lhzu r8,2 (r9)', 'lhzu r8,2(r9) ',
                 'lhzu. r8,2(r9)', 'lhzux r8,r9,r10', 'LHZU r8,2(r9)']
        functions = [(f'bad{i}', 0x1000 + i * 0x10, '\t// ' + form)
                     for i, form in enumerate(forms)]
        functions.append(('unlogged', 0x2000, '\t// lhzu r8,2(r9)'))
        self.prepare(functions)
        self.log.write_text(''.join(
            f'Unrecognized instruction at 0x{0x1000 + i * 0x10:X}: lhzu\n'
            for i in range(len(forms))))
        result, report = self.run_generation()
        self.assertNotIn('sfr::load_halfword_update(', result)
        self.assertEqual(report['counts']['retained_lhzu'], 0)
        self.assertEqual(report['counts']['rejection_reasons']['unsupported_lhzu'],
                         len(forms) + 1)

    def test_lhzu_requires_exact_logs_and_an_empty_instruction_block(self):
        functions = [
            ('mixed', 0x1000, '\t// lhzu r8,2(r9)'),
            ('emitted', 0x1010, '\t// lhzu r8,2(r9)\n\tctx.r8.u64 = 0;'),
            ('different', 0x1020, '\t// nop '),
        ]
        self.prepare(functions)
        self.log.write_text(
            'Unrecognized instruction at 0x1000: lhzu\n'
            'Unable to decode instruction A1090002 at 1000\n'
            'Unrecognized instruction at 0x1010: lhzu\n'
            'Unrecognized instruction at 0x1020: lhzu\n')
        result, report = self.run_generation()
        self.assertNotIn('sfr::load_halfword_update(', result)
        self.assertTrue(all('unsupported_lhzu' in item['reasons']
                            for item in report['rejected_functions']))
        self.assertEqual(report['rejected_functions'][0]['invalid_lhzu_addresses'],
                         ['0x00001000'])

    def test_overlapping_functions_validate_lhzu_independently(self):
        self.prepare([('outer', 0x1000, '\t// nop \n\t// lhzu r8,2(r9)\n\t// blr \n\treturn;'),
                      ('inner', 0x1004, '\t// lhzu r8,2(r9)\n\tctx.r8.u64 = 0;\n'
                       '\t// blr \n\treturn;')])
        self.log.write_text('Unrecognized instruction at 0x1004: lhzu\n')
        result, report = self.run_generation()
        self.assertEqual(result.count('sfr::load_halfword_update('), 1)
        self.assertEqual(report['counts']['retained_lhzu'], 1)
        self.assertEqual(report['rejected_functions'][0]['name'], 'inner')

    def test_lhzu_sthu_and_subfze_resolve_together_but_other_hazards_remain(self):
        body = ('\t// lhzu r8,-2(r9)\n\t// sthu r10,2(r11)\n'
                '\t// subfze r4,r3\n\t// blr \n\treturn;')
        self.prepare([('entry', 0x1000, body)])
        self.log.write_text('Unrecognized instruction at 0x1000: lhzu\n'
                            'Unrecognized instruction at 0x1004: sthu\n'
                            'Unrecognized instruction at 0x1008: subfze\n')
        result, report = self.run_generation()
        self.assertIn('sfr::load_halfword_update(', result)
        self.assertIn('sfr::store_halfword_update(', result)
        self.assertIn('sfr::subfze(', result)
        self.assertEqual(report['counts']['retained_functions'], 1)
        self.output = self.root / 'lhzu-other-hazard'
        self.log.write_text(self.log.read_text() +
                            'Unrecognized instruction at 0x100C: unknown\n')
        result, report = self.run_generation()
        self.assertNotIn('sfr::load_halfword_update(', result)
        self.assertEqual(report['counts']['retained_lhzu'], 0)
        self.assertEqual(report['rejected_functions'][0]['retained_lhzu_addresses'],
                         ['0x00001000'])

    def test_mftb_uses_checked_clock_hook_and_preserves_source(self):
        self.prepare([('clock_read', 0x1000,
                       '\t// mftb r0\n\tctx.r0.u64 = __rdtsc();\n'
                       '\t// mftb r31\n\tctx.r31.u64 = __rdtsc();\n'
                       '\t// blr \n\treturn;')])
        original = (self.source / 'ppc_recomp.0.cpp').read_bytes()
        result, report = self.run_generation()
        self.assertIn('ctx.r0.u64 = sfr::read_time_base();', result)
        self.assertIn('ctx.r31.u64 = sfr::read_time_base();', result)
        self.assertNotIn('__rdtsc', result)
        self.assertEqual(report['counts']['time_base_reads'], 2)
        self.assertEqual(report['counts']['retained_functions'], 1)
        self.assertEqual((self.source / 'ppc_recomp.0.cpp').read_bytes(), original)

    def test_mismatched_time_base_emission_stops_explicitly(self):
        bodies = ['\t// mftbu r3\n\tctx.r3.u64 = __rdtsc();',
                  '\t// mftb r3\n\tctx.r4.u64 = __rdtsc();',
                  '\t// mftb r32\n\tctx.r32.u64 = __rdtsc();',
                  '\t// mftb r3\n\tctx.r3.u64 = 0;',
                  '\t// li r3,0\n\tctx.r3.u64 = __rdtsc();',
                  '\t// mftb r3\n\tctx.r3.u64 = __rdtscp();']
        self.prepare([(f'badclock{i}', 0x1000 + i * 0x10, body)
                      for i, body in enumerate(bodies)])
        result, report = self.run_generation()
        self.assertNotIn('__rdtsc', result)
        self.assertEqual(report['counts']['time_base_reads'], 0)
        self.assertEqual(report['counts']['rejected_functions'], len(bodies))
        for function_report in report['rejected_functions']:
            self.assertIn('unsupported_time_base', function_report['reasons'])

    def test_reservation_pairs_use_checked_hooks_and_preserve_labels(self):
        self.prepare([('atomic0', 0x1000, 'loc_1000:\n' + reservation_pair() +
                       '\t// bne 0x1000\n\tif (!ctx.cr0.eq) goto loc_1000;\n\t// blr \n\treturn;'),
                      ('atomic1', 0x2000, reservation_pair(31, 'r7', 0) + '\t// blr \n\treturn;')])
        source = self.source / 'ppc_recomp.0.cpp'
        source.write_bytes(source.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n'))
        original = source.read_bytes()
        result, report = self.run_generation()
        self.assertIn('ctx.r10.u64 = sfr::load_reserved_word(ctx, uint32_t(ctx.r8.u32));', result)
        self.assertIn('sfr::store_conditional_word(ctx, uint32_t(ctx.r8.u32), ctx.r10.u32);', result)
        self.assertIn('ctx.r31.u64 = sfr::load_reserved_word(ctx, uint32_t(ctx.r7.u32 + ctx.r0.u32));', result)
        self.assertIn('sfr::store_conditional_word(ctx, uint32_t(ctx.r7.u32 + ctx.r0.u32), ctx.r31.u32);', result)
        self.assertIn('loc_1000:', result)
        self.assertNotIn('base +', result)
        self.assertNotIn('ctx.reserved', result)
        self.assertEqual(report['counts']['reservation_loads'], 2)
        self.assertEqual(report['counts']['conditional_stores'], 2)
        self.assertEqual(report['counts']['retained_functions'], 2)
        self.assertEqual(source.read_bytes(), original)

    def test_doubleword_reservations_preserve_full_width_and_index_registers(self):
        self.prepare([('double0', 0x1000, 'loc_1000:\n' + reservation_pair(4, '0', 31, 64) +
                       '\t// bne 0x1000\n\tif (!ctx.cr0.eq) goto loc_1000;\n\t// blr \n\treturn;'),
                      ('double1', 0x2000, reservation_pair(31, 'r7', 0, 64) + '\t// blr \n\treturn;'),
                      ('double2', 0x3000, reservation_pair(0, 'r0', 0, 64) + '\t// blr \n\treturn;')])
        source = self.source / 'ppc_recomp.0.cpp'
        source.write_bytes(source.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n'))
        original = source.read_bytes()
        result, report = self.run_generation()
        self.assertIn('ctx.r4.u64 = sfr::load_reserved_doubleword(ctx, uint32_t(ctx.r31.u32));', result)
        self.assertIn('sfr::store_conditional_doubleword(ctx, uint32_t(ctx.r7.u32 + ctx.r0.u32), ctx.r31.u64);', result)
        self.assertIn('ctx.r0.u64 = sfr::load_reserved_doubleword(ctx, uint32_t(ctx.r0.u32));', result)
        self.assertIn('loc_1000:\n\tsfr::guest_checkpoint();', result)
        self.assertNotIn('ctx.reserved', result)
        self.assertNotIn('__sync_bool_compare_and_swap', result)
        self.assertEqual(report['counts']['retained_functions'], 3)
        self.assertEqual(report['counts']['reservation_loads'], 3)
        self.assertEqual(report['counts']['conditional_stores'], 3)
        self.assertEqual(source.read_bytes(), original)

    def test_doubleword_reservations_reject_changed_emission_and_mixed_only_pairs(self):
        pair = reservation_pair(width=64)
        word = reservation_pair()
        bodies = [pair.replace('uint64_t*', 'uint32_t*'),
                  pair.replace('ctx.r10.s64));', 'ctx.r9.s64));'),
                  pair.replace('ctx.cr0.so = ctx.xer.so;', 'ctx.cr0.so = 0;'),
                  pair.replace('// stdcx.', '// stdcx'),
                  pair.replace('// ldarx r10', '// ldarx r32'),
                  pair.split('\t// stdcx.')[0],
                  '\t// stdcx.' + pair.split('\t// stdcx.')[1],
                  word.split('\t// stwcx.')[0] + '\t// stdcx.' + pair.split('\t// stdcx.')[1],
                  pair.split('\t// stdcx.')[0] + '\t// stwcx.' + word.split('\t// stwcx.')[1]]
        self.prepare([(f'baddouble{i}', 0x1000 + i * 0x100, body) for i, body in enumerate(bodies)])
        result, report = self.run_generation()
        self.assertEqual(report['counts']['rejected_functions'], len(bodies))
        self.assertNotIn('sfr::load_reserved_doubleword(', result)
        self.assertNotIn('sfr::store_conditional_doubleword(', result)

    def test_reservation_blocks_reject_trailing_mutations(self):
        bodies = []
        for width in (32, 64):
            pair = reservation_pair(width=width)
            bodies.extend([
                pair.replace('ctx.cr0.so = ctx.xer.so;\n', 'ctx.cr0.so = ctx.xer.so;\n\tctx.cr0.eq = 1;\n'),
                pair.replace(f'__builtin_bswap{width}(ctx.reserved.u{width});\n',
                             f'__builtin_bswap{width}(ctx.reserved.u{width});\n\tctx.r10.u64 = 0;\n')])
        self.prepare([(f'extrareservation{i}', 0x1000 + i * 0x100, body) for i, body in enumerate(bodies)])
        result, report = self.run_generation()
        self.assertEqual(report['counts']['rejected_functions'], len(bodies))
        for item in report['rejected_functions']:
            self.assertIn('unsupported_reservation', item['reasons'])

    def test_unrecognized_or_incomplete_reservations_remain_stops(self):
        pair = reservation_pair()
        bodies = [pair.replace('ctx.r10.u64 =', 'ctx.r11.u64 ='),
                  pair.replace('base + ctx.r8.u32', 'base + ctx.r9.u32'),
                  pair.replace('ctx.r10.s32));', 'ctx.r9.s32));'),
                  pair.replace('ctx.cr0.so = ctx.xer.so;', 'ctx.cr0.so = 0;'),
                  pair.replace('// lwarx', '// ldarx'),
                  pair.replace('// stwcx.', '// stdcx.'),
                  pair.split('\t// stwcx.')[0],
                  '\t// stwcx.' + pair.split('\t// stwcx.')[1]]
        self.prepare([(f'badreservation{i}', 0x1000 + i * 0x100, body)
                      for i, body in enumerate(bodies)])
        result, report = self.run_generation()
        self.assertEqual(report['counts']['rejected_functions'], len(bodies))
        for function_report in report['rejected_functions']:
            self.assertIn('unsupported_reservation', function_report['reasons'])
        self.assertEqual(report['counts']['reservation_loads'], 0)
        self.assertEqual(report['counts']['conditional_stores'], 0)
        self.assertNotIn('__sync_bool_compare_and_swap', result)

    def test_hazards_and_missing_import_become_traps(self):
        cases = [('\t// tw 31,r0,r0\n\t__builtin_debugtrap();', 'debugtrap'),
                 ('\t// blr \n\t// ERROR 1010\n\treturn;', 'error_comment'),
                 ('\t// .long 0', 'undecoded_instruction'),
                 ('\t// b 0x1110\n\tgoto loc_1110;', 'undefined_label'),
                 ('\t// bl 0x1110\n\tsub_1110(ctx, base);', 'missing_direct_symbol'),
                 ('\t// lvx v0,r3,r4\n\tctx.r3.u32 = *(base + 12);', 'unchecked_guest_memory'),
                 ('\tPPC_FUNC_PROLOGUE();', 'empty_instruction_range')]
        self.prepare([(f'bad{i}', 0x1000 + 0x20 * i, body)
                      for i, (body, _) in enumerate(cases)], [('__imp__ImportA', 0x2000)])
        result, report = self.run_generation()
        self.assertEqual(result.count('sfr::unsupported_function('), len(cases))
        reasons = {reason for item in report['rejected_functions'] for reason in item['reasons']}
        for _, reason in cases:
            self.assertIn(reason, reasons)
        imports = (self.output / 'imports.cpp').read_text()
        self.assertIn('PPC_FUNC(__imp__ImportA)', imports)
        self.assertNotIn('PPC_FUNC_IMPL(', imports)
        self.assertIn('sfr::dispatch_import(ctx, "__imp__ImportA", 0x00002000)', imports)

    def test_decode_failure_is_parsed_and_rejected(self):
        self.prepare([('entry', 0x1000, '\t// nop \n\t// nop ')])
        self.log.write_text('Unable to decode instruction DEADBEEF at 1004\n')
        _, report = self.run_generation()
        self.assertIn('logged_unsupported_instruction', report['rejected_functions'][0]['reasons'])

    def test_missing_condition_result_is_completed_for_known_record_forms(self):
        self.prepare([('entry', 0x1000, '\t// rldicl. r3,r4,0,32\n\tctx.r3.u32 = ctx.r4.u32;')])
        self.log.write_text('rldicl. at 1000 has RC bit enabled but no comparison was generated\n')
        result, report = self.run_generation()
        self.assertNotIn('sfr::unsupported_function', result)
        self.assertIn('ctx.r3.u32 = ctx.r4.u32;\n\tctx.cr0.compare<int64_t>(ctx.r3.s64, 0, ctx.xer);', result)
        self.assertEqual(report['log']['missing_comparison_events'], 1)

    def test_missing_condition_result_is_also_unsupported(self):
        self.prepare([('entry', 0x1000, '\t// rlwnm. r3,r4,r5,0,31\n\tctx.r3.u32 = ctx.r4.u32;')])
        self.log.write_text('rlwnm. at 1000 has RC bit enabled but no comparison was generated\n')
        result, report = self.run_generation()
        self.assertIn('sfr::unsupported_function', result)
        self.assertEqual(report['log']['missing_comparison_events'], 1)

    def test_instruction_positions_are_cross_checked_against_labels(self):
        self.prepare([('entry', 0x1000, '\t// nop \nloc_1020:\n\t// blr \n\treturn;')])
        _, report = self.run_generation()
        self.assertIn('uncertain_instruction_range', report['rejected_functions'][0]['reasons'])

    def test_unexpected_comment_indentation_cannot_hide_overlapping_hazard(self):
        for indentation in ('    ', '\t\t'):
            with self.subTest(indentation=repr(indentation)):
                if self.output.exists():
                    shutil.rmtree(self.output)
                self.prepare([
                    ('outer', 0x1000, '\t// nop \n' + indentation + '// nop \n'
                     '\t// subfze r4,r3'),
                    ('inner', 0x1008, '\t// subfze r4,r3')])
                self.log.write_text('Unrecognized instruction at 0x1008: subfze\n')
                result, report = self.run_generation()
                self.assertEqual(result.count('sfr::unsupported_function('), 1)
                self.assertEqual(result.count('sfr::subfze('), 1)
                outer = next(item for item in report['rejected_functions'] if item['name'] == 'outer')
                self.assertIn('uncertain_instruction_range', outer['reasons'])

    def test_unmapped_log_event_never_publishes_output(self):
        self.prepare([('entry', 0x1000, '\t// blr \n\treturn;')])
        self.log.write_text('Unrecognized instruction at 0x2000: subfze\n')
        with self.assertRaisesRegex(ValueError, 'Unmapped unsupported'):
            generate(self.source, self.log, self.output)
        self.assertFalse(self.output.exists())

    def test_unknown_log_diagnostic_fails_closed(self):
        self.prepare([('entry', 0x1000, '\t// blr \n\treturn;')])
        self.log.write_text('New emitter diagnostic that this filter does not understand\n')
        with self.assertRaisesRegex(ValueError, 'Unparsed diagnostic'):
            generate(self.source, self.log, self.output)
        self.assertFalse(self.output.exists())

    def test_malformed_generation_never_publishes_output(self):
        self.prepare([('entry', 0x1000, '\t// blr \n\treturn;')])
        source = self.source / 'ppc_recomp.0.cpp'
        source.write_text(source.read_text().replace('PPC_WEAK_FUNC(entry)', 'PPC_WEAK_FUNC(wrong)'))
        with self.assertRaises(ValueError):
            generate(self.source, self.log, self.output)
        self.assertFalse(self.output.exists())

    def test_unterminated_body_unknown_mapping_and_bad_log_fail_closed(self):
        for kind in ('unterminated', 'missing_mapping', 'bad_log'):
            with self.subTest(kind=kind):
                self.prepare([('entry', 0x1000, '\t// blr \n\treturn;')])
                if kind == 'unterminated':
                    path = self.source / 'ppc_recomp.0.cpp'
                    path.write_text(path.read_text().rstrip()[:-1])
                elif kind == 'missing_mapping':
                    path = self.source / 'ppc_func_mapping.cpp'
                    path.write_text(path.read_text().replace('entry', 'wrong'))
                else:
                    self.log.write_text('Unrecognized instruction at garbage\n')
                with self.assertRaises(ValueError):
                    generate(self.source, self.log, self.output)
                self.assertFalse(self.output.exists())

    def test_existing_output_and_input_output_nesting_are_rejected(self):
        self.prepare([('entry', 0x1000, '\t// blr \n\treturn;')])
        self.output.mkdir()
        (self.output / 'keep.txt').write_text('keep')
        with self.assertRaises((ValueError, FileExistsError)):
            generate(self.source, self.log, self.output)
        self.assertEqual((self.output / 'keep.txt').read_text(), 'keep')
        with self.assertRaises(ValueError):
            generate(self.source, self.log, self.source / 'diagnostic')

    def test_transient_windows_publication_failure_recovers(self):
        self.prepare([('entry', 0x1000, '\t// blr \n\treturn;')])
        original = Path.rename
        attempts = []
        host_failures = []
        def rename(source, destination):
            attempts.append(source)
            if len(attempts) == 1:
                error = PermissionError('transient directory lock')
                error.winerror = 5
                raise error
            try:
                return original(source, destination)
            except PermissionError as error:
                host_failures.append(error)
                raise
        with patch.object(Path, 'rename', rename), patch.object(diagnostic.sys, 'platform', 'win32'), \
                patch.object(diagnostic.time, 'sleep', wraps=diagnostic.time.sleep) as sleep:
            self.run_generation()
        # The injected failure is deterministic; Windows may independently hold
        # the real staging directory after its files close. Account for those
        # observed failures without relaxing the production retry bound.
        self.assertEqual(len(attempts), 2 + len(host_failures))
        self.assertLessEqual(len(attempts), 5)
        self.assertEqual(sleep.call_count, 1 + len(host_failures))
        for error in host_failures:
            self.assertIn(getattr(error, 'winerror', None), (5, 32, 33))
        self.assertTrue((self.output / 'report.json').is_file())

    def test_persistent_windows_publication_failure_is_bounded(self):
        self.prepare([('entry', 0x1000, '\t// blr \n\treturn;')])
        error = PermissionError('permanent access denial')
        error.winerror = 5
        with patch.object(Path, 'rename', side_effect=error) as rename, \
                patch.object(diagnostic.sys, 'platform', 'win32'), patch.object(diagnostic.time, 'sleep'):
            with self.assertRaises(PermissionError):
                generate(self.source, self.log, self.output)
        self.assertEqual(rename.call_count, 5)
        self.assertFalse(self.output.exists())
        self.assertFalse(list(self.root.glob('diagnostic.staging-*')))

    def test_publication_does_not_retry_over_new_destination(self):
        self.prepare([('entry', 0x1000, '\t// blr \n\treturn;')])
        def rename(source, destination):
            destination.mkdir()
            (destination / 'keep.txt').write_text('keep')
            error = PermissionError('destination appeared')
            error.winerror = 5
            raise error
        with patch.object(Path, 'rename', rename), patch.object(diagnostic.sys, 'platform', 'win32'), \
                patch.object(diagnostic.time, 'sleep') as sleep:
            with self.assertRaises(PermissionError):
                generate(self.source, self.log, self.output)
        sleep.assert_not_called()
        self.assertEqual((self.output / 'keep.txt').read_text(), 'keep')

    def test_non_windows_publication_error_is_not_retried(self):
        self.prepare([('entry', 0x1000, '\t// blr \n\treturn;')])
        with patch.object(Path, 'rename', side_effect=PermissionError('denied')) as rename, \
                patch.object(diagnostic.sys, 'platform', 'linux'), patch.object(diagnostic.time, 'sleep') as sleep:
            with self.assertRaises(PermissionError):
                generate(self.source, self.log, self.output)
        self.assertEqual(rename.call_count, 1)
        sleep.assert_not_called()


if __name__ == '__main__':
    unittest.main()
