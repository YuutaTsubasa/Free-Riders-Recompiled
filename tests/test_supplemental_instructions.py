from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from generate_diagnostic import SUPPLEMENTAL_INSTRUCTIONS, rewrite_supplemental

BASE = 0x82300000


def function(*instructions, newline='\n'):
    body = '\tPPC_FUNC_PROLOGUE();\n' + ''.join(f'\t// {text}\n{emitted}' for text, emitted in instructions)
    return body.replace('\n', newline)


def logged(opcode, index=0):
    return {BASE + 4 * index: [('unrecognized', opcode)]}


class SupplementalInstructionTests(unittest.TestCase):
    def rewrite(self, text, events=None, emitted=''):
        opcode = text.split()[0]
        return rewrite_supplemental(function((text, emitted), ('blr ', '\treturn;\n')), BASE,
                                    logged(opcode) if events is None else events)

    def test_indexed_load_update(self):
        body, resolved, invalid = self.rewrite('lbzux r6,r7,r10')
        self.assertEqual((resolved, invalid), ({BASE}, []))
        self.assertIn('\t// lbzux r6,r7,r10\n\tsfr::load_update<uint8_t>(*sfr::active_memory, '
                      'ctx.r6.u64, ctx.r7.u64, ctx.r10.u64);\n\t// blr', body)

    def test_displacement_forms_sign_extend_the_offset(self):
        body, _, _ = self.rewrite('lhau r10,-2(r11)')
        self.assertIn('sfr::load_update<int16_t>(*sfr::active_memory, ctx.r10.u64, ctx.r11.u64, '
                      'uint64_t(int64_t(-2)));', body)
        body, _, _ = self.rewrite('lfsu f13,292(r10)')
        self.assertIn('sfr::load_single_update(*sfr::active_memory, ctx.f13.u64, ctx.r10.u64, '
                      'uint64_t(int64_t(292)));', body)
        body, _, _ = self.rewrite('stfdu f13,8(r10)')
        self.assertIn('sfr::store_update<uint64_t>(*sfr::active_memory, ctx.r10.u64, ctx.f13.u64, '
                      'uint64_t(int64_t(8)));', body)

    def test_invalid_register_forms_are_rejected(self):
        for text in ('lwzux r10,r10,r4', 'lbzux r6,r0,r10', 'lhau r3,0(r0)', 'stbux r3,r0,r4'):
            with self.subTest(text=text):
                body, resolved, invalid = self.rewrite(text)
                self.assertEqual((resolved, invalid), (set(), [BASE]))
                self.assertNotIn('sfr::', body)

    def test_store_source_may_be_the_base(self):
        _, resolved, _ = self.rewrite('stbux r31,r31,r11')
        self.assertEqual(resolved, {BASE})

    def test_vector_operations_and_record_forms(self):
        body, _, _ = self.rewrite('vslh v25,v1,v30')
        self.assertIn('\tsfr::vmx::vslh(ctx.v25, ctx.v1, ctx.v30);\n', body)
        body, _, _ = self.rewrite('vcmpequh. v13,v0,v4')
        self.assertIn('sfr::vmx::vcmpequh(ctx.v13, ctx.v0, ctx.v4); '
                      'sfr::vmx::set_compare_cr6(ctx.cr6, ctx.v13);', body)
        body, _, _ = self.rewrite('vsel128 v126,v10,v11,v126')
        self.assertIn('sfr::vmx::vsel(ctx.v126, ctx.v10, ctx.v11, ctx.v126);', body)
        body, _, _ = self.rewrite('vspltish v0,-16')
        self.assertIn('sfr::vmx::vspltish(ctx.v0, int16_t(-16));', body)
        body, _, _ = self.rewrite('vcfpuxws128 v63,v62,31')
        self.assertIn('sfr::vmx::vcfpuxws(ctx.v63, ctx.v62, 31);', body)
        _, resolved, invalid = self.rewrite('vcfpuxws128 v63,v62,32')
        self.assertEqual((resolved, invalid), (set(), [BASE]))

    def test_out_of_range_operands_are_rejected(self):
        for text in ('vspltish v0,16', 'vslh v128,v1,v2', 'lhau r3,32768(r4)'):
            with self.subTest(text=text):
                _, resolved, invalid = self.rewrite(text)
                self.assertEqual((resolved, invalid), (set(), [BASE]))

    def test_only_logged_empty_blocks_are_rewritten(self):
        body, resolved, invalid = self.rewrite('vslh v1,v2,v3', events={})
        self.assertEqual((resolved, invalid), (set(), []))  # translated upstream: leave it
        self.assertNotIn('sfr::', body)
        _, resolved, invalid = self.rewrite('vslh v1,v2,v3', events={BASE: [('unrecognized', 'vsrh')]})
        self.assertEqual((resolved, invalid), (set(), []))
        _, resolved, invalid = self.rewrite('vslh v1,v2,v3', emitted='\tctx.r3.u64 = 0;\n')
        self.assertEqual((resolved, invalid), (set(), [BASE]))  # unexpected emitted code

    def test_label_after_the_instruction_is_allowed_and_crlf_kept(self):
        body = function(('lwzux r8,r11,r4', ''), ('blr ', ''), newline='\r\n')
        body = body.replace('\t// blr', 'loc_82300004:\r\n\t// blr')
        rewritten, resolved, _ = rewrite_supplemental(body, BASE, logged('lwzux'))
        self.assertEqual(resolved, {BASE})
        self.assertNotIn('\n', rewritten.replace('\r\n', ''))
        self.assertLess(rewritten.index('sfr::load_update'), rewritten.index('loc_82300004:'))

    def test_every_entry_has_a_c_plus_plus_helper_name(self):
        helpers = (Path(__file__).resolve().parents[1] / 'src/vector_integer.h').read_text()
        for opcode in SUPPLEMENTAL_INSTRUCTIONS:
            if opcode.startswith('v') and opcode not in ('vsel128', 'vspltish', 'vcfpuxws128'):
                # VMX128 encodings share the helper of the plain instruction.
                name = opcode.rstrip('.').removesuffix('128')
                with self.subTest(opcode=opcode):
                    self.assertTrue(any(form in helpers for form in (
                        f'SFR_VMX_LANEWISE({name},', f'SFR_VMX_PACK({name},', f'void {name}(')))
