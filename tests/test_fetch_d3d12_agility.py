"""Offline checks for the pinned D3D12 Agility SDK runtime."""
import hashlib
import importlib.util
import io
from pathlib import Path
import shutil
import unittest
import uuid
from unittest import mock
import zipfile


SPEC = importlib.util.spec_from_file_location(
    'fetch_d3d12_agility', Path(__file__).resolve().parents[1] / 'scripts/fetch_d3d12_agility.py')
fetcher = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(fetcher)


def package_bytes():
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, 'w') as archive:
        archive.writestr('build/native/bin/x64/D3D12Core.dll', b'runtime')
        archive.writestr('LICENSE.txt', b'licence')
    return buffer.getvalue()


class FetchD3D12AgilityTests(unittest.TestCase):
    def setUp(self):
        test_root = Path(__file__).resolve().parent
        self.root = test_root / ('agility-fetch-' + uuid.uuid4().hex)
        self.root.mkdir()
        self.addCleanup(shutil.rmtree, self.root)
        patch = mock.patch.object(fetcher, 'TARGET', self.root)
        patch.start()
        self.addCleanup(patch.stop)

    def serve(self, payload):
        package = dict(fetcher.PACKAGE, sha256=hashlib.sha256(payload).hexdigest())
        mock.patch.dict(fetcher.PACKAGE, package).start()
        self.addCleanup(mock.patch.stopall)
        return mock.patch.object(fetcher.urllib.request, 'urlopen', return_value=io.BytesIO(payload))

    def test_unpacks_the_runtime_licence_and_version(self):
        with self.serve(package_bytes()):
            self.assertTrue(fetcher.fetch())
        self.assertEqual((self.root / 'D3D12Core.dll').read_bytes(), b'runtime')
        self.assertEqual((self.root / 'LICENSE.txt').read_bytes(), b'licence')
        self.assertEqual((self.root / 'version.txt').read_text().strip(), str(fetcher.PACKAGE['sdk_version']))
        self.assertFalse((self.root / 'agility.nupkg').exists())
        self.assertTrue(fetcher.present())
        self.assertFalse(fetcher.fetch(), 'an unpacked runtime is not fetched again')

    def test_refuses_a_package_that_differs_from_the_pin(self):
        with self.serve(package_bytes()), mock.patch.dict(fetcher.PACKAGE, sha256='0' * 64):
            with self.assertRaises(ValueError):
                fetcher.fetch()
        self.assertFalse(fetcher.present())

    def test_another_version_counts_as_missing(self):
        for name in ('D3D12Core.dll', 'LICENSE.txt'):
            (self.root / name).write_bytes(b'x')
        (self.root / 'version.txt').write_text('1\n')
        self.assertFalse(fetcher.present())


if __name__ == '__main__':
    unittest.main()
