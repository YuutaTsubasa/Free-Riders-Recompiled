"""Exercise the real Android archive writer without requiring a device or SDK."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def java_tool(name):
    if os.environ.get('JAVA_HOME'):
        candidate = Path(os.environ['JAVA_HOME']) / 'bin' / (name + ('.exe' if os.name == 'nt' else ''))
        if candidate.is_file():
            return str(candidate)
    return shutil.which(name)


class AndroidDiagnosticsTests(unittest.TestCase):
    @unittest.skipUnless(java_tool('javac') and java_tool('java'), 'JDK required')
    def test_archive_boundaries_and_contents(self):
        with tempfile.TemporaryDirectory() as directory:
            subprocess.run([java_tool('javac'), '-encoding', 'UTF-8', '-d', directory,
                            str(ROOT / 'android/app/src/main/java/com/freeriders/recompiled/DiagnosticsArchive.java'),
                            str(ROOT / 'tests/java/DiagnosticsArchiveTest.java')], check=True)
            subprocess.run([java_tool('java'), '-cp', directory,
                            'com.freeriders.recompiled.DiagnosticsArchiveTest', directory], check=True)


if __name__ == '__main__':
    unittest.main()
