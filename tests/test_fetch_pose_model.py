"""Offline checks for pinned camera-model downloads."""
import hashlib
import importlib.util
import io
from pathlib import Path
import shutil
import unittest
import uuid
from unittest import mock


SPEC = importlib.util.spec_from_file_location(
    'fetch_pose_model', Path(__file__).resolve().parents[1] / 'scripts/fetch_pose_model.py')
fetcher = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(fetcher)


class FetchPoseModelTests(unittest.TestCase):
    def setUp(self):
        # Default mkdir permissions retain the Windows sandbox's inherited ACL.
        test_root = Path(__file__).resolve().parent
        self.root = test_root / ('pose-fetch-' + uuid.uuid4().hex)
        self.root.mkdir()
        self.assertEqual(self.root.resolve().parent, test_root)
        self.addCleanup(shutil.rmtree, self.root)
        self.tools_patch = mock.patch.object(fetcher, 'TOOLS', self.root)
        self.tools_patch.start()
        self.addCleanup(self.tools_patch.stop)
        self.payload = b'pinned model bytes'
        self.entry = {'url': 'https://example.invalid/model.onnx',
                      'sha256': hashlib.sha256(self.payload).hexdigest()}

    def test_nested_model_download_is_verified_and_installed(self):
        with mock.patch.object(fetcher.urllib.request, 'urlopen', return_value=io.BytesIO(self.payload)):
            result = fetcher.fetch('mediapipe/model.onnx', self.entry, False)
        self.assertEqual(result.read_bytes(), self.payload)
        self.assertFalse(result.with_suffix('.onnx.partial').exists())

    def test_existing_valid_model_never_uses_network(self):
        model = self.root / 'model.onnx'
        model.write_bytes(self.payload)
        with mock.patch.object(fetcher.urllib.request, 'urlopen') as network:
            self.assertEqual(fetcher.fetch('model.onnx', self.entry, False), model)
            network.assert_not_called()

    def test_verify_only_rejects_missing_or_altered_files_without_network(self):
        with mock.patch.object(fetcher.urllib.request, 'urlopen') as network:
            for contents in (None, b'altered model'):
                with self.subTest(contents=contents):
                    if contents is not None:
                        (self.root / 'model.onnx').write_bytes(contents)
                    with self.assertRaisesRegex(SystemExit, 'missing or altered download'):
                        fetcher.fetch('model.onnx', self.entry, True)
            network.assert_not_called()

    def test_invalid_download_preserves_existing_file(self):
        model = self.root / 'model.onnx'
        model.write_bytes(b'previous file')
        with mock.patch.object(fetcher.urllib.request, 'urlopen', return_value=io.BytesIO(b'bad download')):
            with self.assertRaisesRegex(SystemExit, 'sha256'):
                fetcher.fetch('model.onnx', self.entry, False)
        self.assertEqual(model.read_bytes(), b'previous file')
        self.assertFalse(model.with_suffix('.onnx.partial').exists())

    def test_default_verification_includes_runtime_both_models_and_license(self):
        with mock.patch('sys.argv', ['fetch_pose_model.py', '--verify-only']), \
                mock.patch.object(fetcher, 'fetch') as fetch, \
                mock.patch.object(fetcher, 'unpack_runtime') as unpack:
            fetcher.main()
        self.assertEqual({call.args[0] for call in fetch.call_args_list}, {
            'onnxruntime-win-x64.zip',
            'mediapipe/pose_estimation_mediapipe_2023mar.onnx',
            'mediapipe/person_detection_mediapipe_2023mar.onnx',
            'mediapipe/LICENSE',
        })
        self.assertTrue(all(call.args[2] for call in fetch.call_args_list))
        unpack.assert_not_called()

    def test_legacy_selection_only_verifies_runtime_and_rtmpose(self):
        with mock.patch('sys.argv', ['fetch_pose_model.py', '--model', 'rtmpose', '--verify-only']), \
                mock.patch.object(fetcher, 'fetch') as fetch:
            fetcher.main()
        self.assertEqual({call.args[0] for call in fetch.call_args_list},
                         {'onnxruntime-win-x64.zip', 'rtmpose-t.zip'})

    def test_linux_selection_verifies_the_linux_runtime(self):
        with mock.patch('sys.argv', ['fetch_pose_model.py', '--platform', 'linux', '--verify-only']), \
                mock.patch.object(fetcher, 'fetch') as fetch:
            fetcher.main()
        names = {call.args[0] for call in fetch.call_args_list}
        self.assertIn('onnxruntime-linux-x64.tgz', names)
        self.assertNotIn('onnxruntime-win-x64.zip', names)

    def test_android_adds_its_runtime_and_the_notices_it_borrows(self):
        with mock.patch('sys.argv', ['fetch_pose_model.py', '--android', '--verify-only']), \
                mock.patch.object(fetcher, 'fetch') as fetch:
            fetcher.main()
        names = {call.args[0] for call in fetch.call_args_list}
        self.assertTrue({'onnxruntime-win-x64.zip', 'onnxruntime-android.aar',
                         'onnxruntime-linux-x64.tgz'} <= names)


if __name__ == '__main__':
    unittest.main()
