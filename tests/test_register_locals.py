from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import register_locals as rl

ALL = 'ctr,xer,cr,nonargument,nonvolatile'


def program(source, classes=ALL):
    lines = source.split('\n')
    functions = rl.parse(lines, 'test')
    prog = rl.Program(functions, rl.parse_classes(classes))
    prog.solve()
    return prog, functions, lines


def convert(source, classes=ALL):
    prog, functions, _ = program(source, classes)
    return rl.transform_text(source, prog, functions)


def body(text, name):
    start = text.index(f'PPC_FUNC_IMPL(__imp__{name})')
    return text[start:text.index('\n}', start) + 2]


# A caller keeping a value in r31 and cr2 across a call, and calling a
# callee that reads r11 as an input (a fragment entered with it set).
SOURCE = '''#include "ppc_recomp_shared.h"

PPC_FUNC_IMPL(__imp__sub_1000) {
\tPPC_FUNC_PROLOGUE();
\tsfr::enter_function(ctx, "sub_1000", 0x1000);
\t// mflr r12
\tctx.r12.u64 = ctx.lr;
\t// bl 0x82a56068
\tctx.lr = 0x1008;
\t__savegprlr_29(ctx, base);
\tctx.r31.u64 = ctx.r3.u64;
\tctx.cr2.compare<uint32_t>(ctx.r31.u32, 0, ctx.xer);
\tctx.r11.s64 = 5;
\tctx.lr = 0x1018;
\tsub_2000(ctx, base);
\tif (ctx.cr2.eq) goto loc_1024;
\tctx.r3.u64 = ctx.r31.u64;
loc_1024:
\tctx.ctr.u64 = ctx.r4.u64;
\tctx.lr = 0x1030;
\tPPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);
\t// b 0x82a560b4
\t__restgprlr_29(ctx, base);
\treturn;
}

PPC_FUNC_IMPL(__imp__sub_2000) {
\tPPC_FUNC_PROLOGUE();
\tsfr::enter_function(ctx, "sub_2000", 0x2000);
\tctx.r3.u64 = ctx.r11.u64;
\tctx.r31.s64 = 0;
\treturn;
}
'''

# sub_3000 returns a value in r11 to its caller, outside the convention.
RETURNS = '''PPC_FUNC_IMPL(__imp__sub_3000) {
\tPPC_FUNC_PROLOGUE();
\tsfr::enter_function(ctx, "sub_3000", 0x3000);
\tctx.r11.s64 = 7;
\treturn;
}

PPC_FUNC_IMPL(__imp__sub_3100) {
\tPPC_FUNC_PROLOGUE();
\tsfr::enter_function(ctx, "sub_3100", 0x3100);
\tctx.r11.s64 = 1;
\tsub_3000(ctx, base);
\tctx.r3.u64 = ctx.r11.u64;
\treturn;
}

PPC_FUNC_IMPL(__imp__sub_3200) {
\tPPC_FUNC_PROLOGUE();
\tsfr::enter_function(ctx, "sub_3200", 0x3200);
\t__imp__KeBugCheck(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_3300) {
\tPPC_FUNC_PROLOGUE();
\tsfr::enter_function(ctx, "sub_3300", 0x3300);
\tctx.r11.s64 = 1;
\tsub_3200(ctx, base);
loc_3310:
\tctx.r3.u64 = ctx.r11.u64;
\treturn;
}
'''


class RegisterLocalsTest(unittest.TestCase):
    def test_none_leaves_the_code_alone(self):
        prog, functions, _ = program(SOURCE, 'none')
        self.assertEqual(rl.transform_text(SOURCE, prog, functions), SOURCE)

    def test_registers_become_locals(self):
        text = body(convert(SOURCE), 'sub_1000')
        for name in ('r11', 'r12', 'r31', 'cr2', 'ctr', 'xer'):
            self.assertNotIn(f'ctx.{name}.', text.replace(f'ctx.{name} = {name};', ''))
        self.assertIn('\tPPCRegister r31{};', text)
        self.assertIn('\tPPCCRRegister cr2{};', text)
        self.assertIn('\tPPCXERRegister xer{};', text)
        self.assertIn('PPC_CALL_INDIRECT_FUNC(ctr.u32);', text)
        # Arguments and the stack pointer stay in the context.
        self.assertIn('r31.u64 = ctx.r3.u64;', text)
        self.assertIn('ctx.r3.u64 = r31.u64;', text)

    def test_a_callee_input_is_stored_before_the_call(self):
        text = convert(SOURCE)
        caller = body(text, 'sub_1000')
        self.assertIn('\tctx.r11 = r11;\n\tsub_2000(ctx, base);', caller)
        # r31 is the callee's own (it writes it before reading) and cr2 the
        # callee preserves: neither is stored.
        self.assertNotIn('ctx.r31 = r31;', caller)
        self.assertNotIn('ctx.cr2 = cr2;', caller)
        callee = body(text, 'sub_2000')
        self.assertIn('\tPPCRegister r11 = ctx.r11;', callee)
        self.assertIn('\tPPCRegister r31{};', callee)

    def test_nonvolatile_helpers_are_dropped_but_the_return_address_kept(self):
        caller = body(convert(SOURCE), 'sub_1000')
        self.assertNotIn('__savegprlr_29', caller)
        self.assertNotIn('__restgprlr_29', caller)
        self.assertNotIn('ctx.lr = 0x1008;', caller)
        self.assertIn('\tPPC_STORE_U32(ctx.r1.u32 - 8, r12.u32);', caller)
        self.assertIn('\tctx.lr = PPC_LOAD_U32(ctx.r1.u32 - 8);\n\treturn;', caller)
        # Without the nonvolatile class the helpers stay, with r12 stored first.
        kept = body(convert(SOURCE, 'nonargument'), 'sub_1000')
        self.assertIn('\tctx.r12 = r12;\n\t__savegprlr_29(ctx, base);', kept)
        self.assertIn('__restgprlr_29(ctx, base);', kept)

    def test_a_register_returned_outside_the_convention_goes_through_the_context(self):
        text = convert(RETURNS)
        self.assertIn('\tctx.r11 = r11;\n\treturn;', body(text, 'sub_3000'))
        caller = body(text, 'sub_3100')
        self.assertIn('\tsub_3000(ctx, base);\n\tr11 = ctx.r11;', caller)

    def test_code_after_a_call_that_never_returns_asks_nothing(self):
        prog, functions, _ = program(RETURNS)
        self.assertIn('sub_3200', prog.noreturn)
        self.assertEqual(prog.functions['sub_3200'].ret, 0)

    def test_prologue_saves_and_mfcr_are_not_inputs(self):
        source = '''PPC_FUNC_IMPL(__imp__sub_4000) {
\tPPC_FUNC_PROLOGUE();
\tsfr::enter_function(ctx, "sub_4000", 0x4000);
\tPPC_STORE_U64(ctx.r1.u32 + -16, ctx.r31.u64);
\tctx.r12.u64 = ctx.cr0.lt ? 0x80000000 : 0;
\tctx.r12.u64 |= ctx.cr2.eq ? 0x200000 : 0;
\tPPC_STORE_U32(ctx.r1.u32 + -20, ctx.r12.u32);
\tctx.r31.u64 = ctx.r3.u64;
\tctx.r31.u64 = PPC_LOAD_U64(ctx.r1.u32 + -16);
\treturn;
}
'''
        prog, functions, _ = program(source)
        self.assertEqual(prog.functions['sub_4000'].livein, 0)

    def test_cr_bits_are_tracked_one_by_one(self):
        source = '''PPC_FUNC_IMPL(__imp__sub_5000) {
\tPPC_FUNC_PROLOGUE();
\tsfr::enter_function(ctx, "sub_5000", 0x5000);
\tctx.cr0.compare<int32_t>(ctx.r3.s32, 0, ctx.xer);
\tctx.cr1.eq = (ctx.cr0.lt | ctx.cr0.eq) & 1;
\tif (ctx.cr1.eq) goto loc_5010;
loc_5010:
\treturn;
}
'''
        prog, functions, _ = program(source)
        self.assertEqual(prog.functions['sub_5000'].livein, 0)

    def test_host_reads_at_a_call_site_are_stored_first(self):
        # The stream reader's idle wait (game_patches.cpp) reads r19 and r30
        # at the call returning to 8249C224, which is made through a pointer.
        source = '''PPC_FUNC_IMPL(__imp__sub_8249C100) {
\tPPC_FUNC_PROLOGUE();
\tsfr::enter_function(ctx, "sub_8249C100", 0x8249C100);
\tctx.r19.u64 = ctx.r3.u64;
\tctx.r30.u64 = ctx.r4.u64;
\tctx.ctr.u64 = ctx.r5.u64;
\tctx.lr = 0x8249C224;
\tPPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);
\tctx.r3.u64 = ctx.r19.u64 + ctx.r30.u64;
\treturn;
}
'''
        text = convert(source)
        self.assertIn('\tctx.r19 = r19;\n\tctx.r30 = r30;\n\tPPC_CALL_INDIRECT_FUNC(ctr.u32);', text)

    def test_unknown_control_flow_is_refused(self):
        source = '''PPC_FUNC_IMPL(__imp__sub_6000) {
\tPPC_FUNC_PROLOGUE();
\twhile (ctx.r3.u32) ctx.r3.u64--;
\treturn;
}
'''
        with self.assertRaises(ValueError):
            rl.parse(source.split('\n'), 'test')

    def test_code_constants_find_addresses(self):
        text = '\tctx.r11.s64 = -2112290816;\n\tctx.r3.s64 = ctx.r11.s64 + 31740;\n'
        self.assertIn((-2112290816 + 31740) & 0xFFFFFFFF, rl.code_constants(text))


if __name__ == '__main__':
    unittest.main()
