import contextlib
import io
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts' / 'pgo_compile.py'
sys.path.insert(0, str(SCRIPT.parent))
import pgo_compile

# A stand-in compiler: prints Clang's hash-mismatch warning N times on stderr.
FAKE = ("import sys\n"
        "for _ in range(int(sys.argv[1])): print('warning: a.cpp: function control flow change detected "
        "(hash mismatch) f Hash = 1 up to 2 count discarded [-Wbackend-plugin]', file=sys.stderr)\n"
        "sys.exit(int(sys.argv[2]))\n")


class PgoCompileTest(unittest.TestCase):
    def run_compiler(self, build, stale, code=0):
        return subprocess.run([sys.executable, str(SCRIPT), sys.executable, '-c', FAKE, str(stale), str(code),
                               '-o', 'obj/a.cpp.o'], cwd=build, capture_output=True, text=True)

    def summary(self, build):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            pgo_compile.summary(build)
        return out.getvalue()

    def test_counts_changed_functions_per_object(self):
        with tempfile.TemporaryDirectory() as build:
            (Path(build) / 'obj').mkdir()
            result = self.run_compiler(build, 3)
            self.assertEqual(result.returncode, 0)
            self.assertEqual(result.stderr.count('hash mismatch'), 3)  # passed through
            self.assertEqual((Path(build) / 'obj/a.cpp.o.pgo-stale').read_text(), '3\n')
            self.assertIn('3 function(s) in 1 file(s)', self.summary(build))

    def test_clean_compile_clears_the_count(self):
        with tempfile.TemporaryDirectory() as build:
            (Path(build) / 'obj').mkdir()
            self.run_compiler(build, 2)
            self.run_compiler(build, 0)
            self.assertFalse((Path(build) / 'obj/a.cpp.o.pgo-stale').exists())
            self.assertEqual(self.summary(build), '')

    def test_failed_compile_keeps_exit_code_and_no_count(self):
        with tempfile.TemporaryDirectory() as build:
            (Path(build) / 'obj').mkdir()
            self.assertEqual(self.run_compiler(build, 1, code=3).returncode, 3)
            self.assertFalse((Path(build) / 'obj/a.cpp.o.pgo-stale').exists())


if __name__ == '__main__':
    unittest.main()
