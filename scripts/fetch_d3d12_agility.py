#!/usr/bin/env python3
"""Fetch the Direct3D 12 Agility SDK runtime the Windows game draws with.

Plume's D3D12 backend uses interfaces (ID3D12GraphicsCommandList7 and the
enhanced barriers) that only a recent D3D12 runtime has: Windows 11 24H2
carries one, Windows 10 does not. The Agility SDK's D3D12Core.dll, beside the
game in D3D12\\, gives any Windows 10 (1909 or later) that runtime; the game
exports the SDK version it expects (src/d3d12_agility.cpp), and CMake copies
the DLL next to it after each build.

The package is a binary artefact, so it is pinned by SHA-256 here, like the
camera's (scripts/fetch_pose_model.py), and unpacked into tools/d3d12-agility
(git-ignored): D3D12Core.dll, the licence, and version.txt with the SDK
version CMake gives the export. bootstrap.py runs this on Windows.

  python scripts/fetch_d3d12_agility.py [--verify-only]
"""
import argparse
import hashlib
from pathlib import Path
import sys
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TARGET = ROOT / 'tools/d3d12-agility'

PACKAGE = {
    'url': 'https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/1.619.6/'
           'microsoft.direct3d.d3d12.1.619.6.nupkg',
    'sha256': '08f0489281401aa430fc37322d6c3fc98a8025175aacd714c10d562f4963f1e9',
    # D3D12_SDK_VERSION of this package: the export must name exactly it.
    'sdk_version': 619,
}
# (name in tools/d3d12-agility, path in the package). D3D12Core.dll is on the
# package's list of files that may be distributed.
FILES = [
    ('D3D12Core.dll', 'build/native/bin/x64/D3D12Core.dll'),
    ('LICENSE.txt', 'LICENSE.txt'),
]


def digest(path):
    hasher = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            hasher.update(block)
    return hasher.hexdigest()


def present():
    version = TARGET / 'version.txt'
    return (all((TARGET / name).is_file() for name, _ in FILES) and version.is_file()
            and version.read_text().strip() == str(PACKAGE['sdk_version']))


def fetch():
    """Unpack the pinned runtime into tools/d3d12-agility unless it is there."""
    if present():
        return False
    TARGET.mkdir(parents=True, exist_ok=True)
    package = TARGET / 'agility.nupkg'
    if not package.is_file() or digest(package) != PACKAGE['sha256']:
        print(f'fetching {PACKAGE["url"]}')
        partial = package.with_suffix('.partial')
        with urllib.request.urlopen(PACKAGE['url']) as response, partial.open('wb') as out:
            while block := response.read(1 << 20):
                out.write(block)
        if digest(partial) != PACKAGE['sha256']:
            partial.unlink()
            raise ValueError('the D3D12 Agility SDK package does not match its pinned SHA-256')
        partial.replace(package)
    with zipfile.ZipFile(package) as archive:
        for name, inside in FILES:
            (TARGET / name).write_bytes(archive.read(inside))
    (TARGET / 'version.txt').write_text(f'{PACKAGE["sdk_version"]}\n')
    package.unlink()
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--verify-only', action='store_true', help='only check that the runtime is unpacked')
    args = parser.parse_args()
    if args.verify_only:
        if not present():
            sys.exit('D3D12 Agility SDK runtime missing: run scripts/fetch_d3d12_agility.py')
    elif fetch():
        print(f'D3D12 Agility SDK {PACKAGE["sdk_version"]} in {TARGET}')
        print('Configure again so CMake copies it beside the game.')
    print(f'Verified D3D12 Agility SDK {PACKAGE["sdk_version"]}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
