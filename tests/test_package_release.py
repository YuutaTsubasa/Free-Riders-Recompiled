"""Release camera dependencies must be complete, opt-in and licensed."""
import importlib.util
from pathlib import Path
import tempfile
import struct
import sys
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location(
    'package_release', Path(__file__).resolve().parents[1] / 'scripts/package_release.py')
package = importlib.util.module_from_spec(SPEC)
with mock.patch.object(sys, 'path', [str(Path(__file__).resolve().parents[1] / 'scripts'), *sys.path]):
    SPEC.loader.exec_module(package)
    from shader_pack_format import SHADER_ABI


class CameraPackageTests(unittest.TestCase):
    def test_camera_bundle_contains_models_runtime_and_notices(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sources = {
                'onnxruntime/lib/onnxruntime.dll': 'onnxruntime.dll',
                'onnxruntime/lib/onnxruntime_providers_shared.dll': 'onnxruntime_providers_shared.dll',
                'onnxruntime/LICENSE': 'licenses/ONNXRuntime-MIT.txt',
                'onnxruntime/ThirdPartyNotices.txt': 'licenses/ONNXRuntime-ThirdPartyNotices.txt',
                'mediapipe/pose_estimation_mediapipe_2023mar.onnx': 'pose/pose_estimation_mediapipe_2023mar.onnx',
                'mediapipe/person_detection_mediapipe_2023mar.onnx': 'pose/person_detection_mediapipe_2023mar.onnx',
                'mediapipe/LICENSE': 'licenses/MediaPipe-Apache-2.0.txt',
            }
            for name in sources:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(name)
            self.assertEqual(dict(package.camera_files(root)),
                             {target: root / source for source, target in sources.items()})
            # A partially installed camera dependency must stop packaging.
            for missing in ('mediapipe/LICENSE', 'mediapipe/person_detection_mediapipe_2023mar.onnx',
                            'onnxruntime/ThirdPartyNotices.txt', 'onnxruntime/lib/onnxruntime.dll'):
                with self.subTest(missing=missing):
                    path = root / missing
                    path.unlink()
                    with self.assertRaises(SystemExit):
                        package.camera_files(root)
                    path.write_text(missing)


class ShaderPackageTests(unittest.TestCase):
    def test_release_rejects_shader_pack_without_current_abi(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ('FreeRidersRecompiled', 'sfr_cpu_diagnostic'):
                (root / name).write_bytes(b'program')
            pack = root / 'shaders.pack'
            for header in (b'SFRSHPK1' + struct.pack('<I', 1),
                           b'SFRSHPK2' + struct.pack('<II', 6, 1)):
                with self.subTest(header=header):
                    pack.write_bytes(header + struct.pack('<5I', 0, 0, 4, 1, 1) + b'gameDS')
                    with self.assertRaisesRegex(SystemExit, 'shader.*(ABI|outdated)'):
                        package.desktop_files('linux', root, pack)

    def test_release_accepts_complete_current_pack_and_rejects_truncation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ('FreeRidersRecompiled', 'sfr_cpu_diagnostic'):
                (root / name).write_bytes(b'program')
            pack = root / 'shaders.pack'
            valid = b'SFRSHPK2' + struct.pack('<II', SHADER_ABI, 1) + struct.pack('<5I', 0, 0, 4, 1, 1) + b'gameDS'
            pack.write_bytes(valid)
            self.assertIn(('shaders.pack', pack), package.desktop_files('linux', root, pack))
            for size in (0, 8, 15, 16, len(valid) - 1):
                with self.subTest(size=size):
                    pack.write_bytes(valid[:size])
                    with self.assertRaises(SystemExit):
                        package.desktop_files('linux', root, pack)


if __name__ == '__main__':
    unittest.main()
