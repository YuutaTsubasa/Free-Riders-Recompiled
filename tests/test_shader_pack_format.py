import re
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
with mock.patch.object(sys, 'path', [str(ROOT / 'scripts'), *sys.path]):
    from shader_pack_format import MAGIC, SHADER_ABI, header, validate_shader_pack


class ShaderPackTests(unittest.TestCase):
    def test_producer_only_uses_current_cache_and_stamps_native_abi(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for version in (8, SHADER_ABI):
                folder = root / f'v{version}-example'
                folder.mkdir()
                (folder / 'original.bin').write_bytes(b'\0\0\0\1')
                (folder / 'specialization_mask.txt').write_text('0')
                (folder / 'shader.spv').write_bytes(bytes([version]))
            output = root / 'shaders.pack'
            subprocess.run([sys.executable, str(ROOT / 'scripts/pack_shaders.py'),
                            '--cache', str(root), '--output', str(output)], check=True, capture_output=True)
            data = output.read_bytes()
            validate_shader_pack(data)
            self.assertEqual(data[:16], b'SFRSHPK2' + struct.pack('<II', SHADER_ABI, 1))
            self.assertEqual(data[-1], SHADER_ABI)
            native = (ROOT / 'src/shader_pack_format.h').read_text()
            self.assertEqual(int(re.search(r'shader_abi_version = (\d+)', native)[1]), SHADER_ABI)
            self.assertIn(MAGIC.decode(), native)

    def test_invalid_entries_and_trailing_data_are_rejected(self):
        entry = struct.pack('<5I', 0, 0, 4, 1, 1) + b'gameDS'
        validate_shader_pack(header(1) + entry)
        for data in (header(0), header(2) + entry, header(1) + entry + b'extra',
                     header(1) + struct.pack('<5I', 2, 0, 4, 1, 1) + b'gameDS',
                     header(1) + struct.pack('<5I', 0, 0, 4, 0, 0) + b'game',
                     header(1) + struct.pack('<5I', 0, 0, 0xFFFFFFFF, 1, 1)):
            with self.subTest(data=data):
                with self.assertRaises(ValueError):
                    validate_shader_pack(data)

    def test_android_packager_rejects_legacy_pack_before_build_tools(self):
        with tempfile.TemporaryDirectory() as directory:
            pack = Path(directory) / 'shaders.pack'
            pack.write_bytes(b'SFRSHPK1' + struct.pack('<I', 1))
            run = subprocess.run([sys.executable, str(ROOT / 'scripts/package_android.py'),
                                  '--pack', str(pack)], capture_output=True, text=True)
            self.assertNotEqual(run.returncode, 0)
            self.assertIn('shader pack is outdated', run.stderr)


if __name__ == '__main__':
    unittest.main()
