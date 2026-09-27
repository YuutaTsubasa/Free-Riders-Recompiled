"""Release camera dependencies must be complete, opt-in and licensed."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    'package_release', Path(__file__).resolve().parents[1] / 'scripts/package_release.py')
package = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(package)


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


if __name__ == '__main__':
    unittest.main()
