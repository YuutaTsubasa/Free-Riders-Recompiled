from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import loop_head_checkpoints as loops

SOURCE = '''#include "diagnostic_hooks.h"

PPC_FUNC_IMPL(__imp__sub_1000) {
\tPPC_FUNC_PROLOGUE();
\tsfr::enter_function(ctx, "sub_1000", 0x1000);
loc_1004:
\tsfr::guest_checkpoint(ctx);
\tif (ctx.cr6.eq) goto loc_1010;
loc_1008:
\tsfr::guest_checkpoint(ctx);
\t--ctx.ctr.u64;
\tif (ctx.ctr.u32 != 0) goto loc_1008;
loc_1010:
\tsfr::guest_checkpoint(ctx);
\tswitch (ctx.r3.u32) {
\tcase 0: goto loc_1004;
\tcase 1: goto loc_1018;
\t}
loc_1018:
\tsfr::guest_checkpoint(ctx);
\treturn;
}

PPC_FUNC_IMPL(__imp__sub_1020) {
\tPPC_FUNC_PROLOGUE();
\tsfr::enter_function(ctx, "sub_1020", 0x1020);
\tgoto loc_1008;
loc_1008:
\tsfr::guest_checkpoint(ctx);
\treturn;
}
'''


class LoopHeadCheckpointTest(unittest.TestCase):
    def test_keeps_only_labels_a_later_goto_returns_to(self):
        result, removed, kept = loops.transform(SOURCE)
        # sub_1000: loc_1004 (switch case 0 below it) and loc_1008 (bdnz) are loop heads;
        # loc_1010 and loc_1018 are only reached forwards.
        self.assertIn('loc_1004:\n\tsfr::guest_checkpoint(ctx);', result)
        self.assertIn('loc_1008:\n\tsfr::guest_checkpoint(ctx);\n\t--ctx.ctr.u64;', result)
        self.assertIn('loc_1010:\n\tswitch', result)
        self.assertIn('loc_1018:\n\treturn;', result)
        # a label of another function with the same name is judged in its own function
        self.assertIn('\tgoto loc_1008;\nloc_1008:\n\treturn;', result)
        self.assertEqual((removed, kept), (3, 2))
        self.assertTrue(loops.check(SOURCE, result))

    def test_check_rejects_any_other_change(self):
        result, _, _ = loops.transform(SOURCE)
        self.assertFalse(loops.check(SOURCE, result.replace('ctx.ctr.u32 != 0', 'ctx.ctr.u32 == 0')))
        self.assertFalse(loops.check(SOURCE, result.replace('\treturn;\n}', '}', 1)))

    def test_directory_copy_and_check(self):
        with tempfile.TemporaryDirectory() as work:
            source, output = Path(work) / 'in', Path(work) / 'out'
            source.mkdir()
            (source / 'ppc_recomp.0.cpp').write_text(SOURCE, newline='')
            (source / 'ppc_context.h').write_text('// header\n', newline='')
            loops.main([str(source), str(output)])
            loops.main(['--check', str(source), str(output)])
            self.assertEqual((output / 'ppc_context.h').read_text(), '// header\n')
            (output / 'ppc_recomp.0.cpp').write_text(SOURCE, newline='')
            with self.assertRaises(SystemExit):
                loops.main(['--check', str(source), str(output)])

    def test_single_file_as_the_build_converts_it(self):
        with tempfile.TemporaryDirectory() as work:
            source, output, again = Path(work) / 'ppc_recomp.0.cpp', Path(work) / 'build/ppc_recomp.0.cpp', Path(work) / 'again.cpp'
            source.write_text(SOURCE, newline='')
            loops.main(['--file', str(source), str(output)])
            written = output.read_bytes().decode('utf-8')
            self.assertEqual(written, loops.transform(SOURCE)[0])
            self.assertTrue(loops.check(SOURCE, written))
            # current generator output (loop heads only) passes through unchanged
            loops.main(['--file', str(output), str(again)])
            self.assertEqual(again.read_bytes(), output.read_bytes())
            with self.assertRaises(SystemExit):
                loops.main(['--file', str(source), str(source)])


if __name__ == '__main__':
    unittest.main()
