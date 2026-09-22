import os
import hashlib
import subprocess
import tempfile
import unittest
from pathlib import Path


@unittest.skipUnless(os.environ.get('SFR_IMAGE_DUMP'), 'set SFR_IMAGE_DUMP for native tool tests')
class ImageDumpTests(unittest.TestCase):
    @unittest.skipUnless(os.environ.get('SFR_SOURCE_XEX'), 'requires verified local XEX')
    def test_exports_original_header_before_completion(self):
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / 'dump'
            result = subprocess.run([os.environ['SFR_IMAGE_DUMP'], os.environ['SFR_SOURCE_XEX'], str(dest)],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((dest / 'xex_header.bin').is_file(), 'original XEX header is missing')
            header = (dest / 'xex_header.bin').read_bytes()
            self.assertEqual(len(header), 0x5000)
            self.assertEqual(hashlib.sha1(header).hexdigest(), '82ef42d3f130b26563fcd85909f2eab613de6c68')
            self.assertTrue((dest / 'complete.txt').is_file())

    def test_rejects_invalid_xex_without_creating_output(self):
        exe = Path(os.environ['SFR_IMAGE_DUMP']).resolve()
        self.assertTrue(exe.is_file(), 'native image tool is not built')
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'bad.xex'
            source.write_bytes(b'XEX2' + bytes(100))
            dest = Path(tmp) / 'dump'
            result = subprocess.run([str(exe), str(source), str(dest)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('unsupported', result.stderr.lower())
            self.assertFalse(dest.exists())

    def test_rejects_same_length_unrecognized_xex(self):
        exe = Path(os.environ['SFR_IMAGE_DUMP']).resolve()
        self.assertTrue(exe.is_file(), 'native image tool is not built')
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'bad.xex'
            with source.open('wb') as stream:
                stream.write(b'XEX2')
                stream.truncate(14241792)
            dest = Path(tmp) / 'dump'
            result = subprocess.run([str(exe), str(source), str(dest)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('unsupported', result.stderr.lower())
            self.assertFalse(dest.exists())
