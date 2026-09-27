#!/usr/bin/env python3
"""Fetch ONNX Runtime and the RTMPose model the camera's motion input needs.

Both are release artefacts rather than repositories, so they are pinned by
SHA-256 here instead of in config/dependencies.lock.json, and unpacked into
tools/onnx (git-ignored):

  tools/onnx/onnxruntime          this host's runtime (Windows or Linux x64)
  tools/onnx/onnxruntime-android  with --android: the runtime for each ABI
  tools/onnx/rtmpose              the model, with its licence

Neither is part of this project's source; a release that carries them
carries their licences too (scripts/package_release.py does).

  python scripts/fetch_pose_model.py [--android] [--verify-only]
"""
import argparse
import hashlib
from pathlib import Path
import shutil
import sys
import tarfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools/onnx'

# ONNX Runtime is MIT (Microsoft); RTMPose is Apache 2.0 (OpenMMLab).
DOWNLOADS = {
    'onnxruntime-win-x64.zip': {
        'url': 'https://github.com/microsoft/onnxruntime/releases/download/v1.30.0/onnxruntime-win-x64-1.30.0.zip',
        'sha256': 'c6ba983baf5681af108599675d2a89c2d145512d02de28aed0bff177cd0ba949',
    },
    'onnxruntime-linux-x64.tgz': {
        'url': 'https://github.com/microsoft/onnxruntime/releases/download/v1.30.0/onnxruntime-linux-x64-1.30.0.tgz',
        'sha256': 'a5ed5a3cac51fbb2e90da632ae43d19212faaa20e76484e62bcb7c23ddb3b3fd',
    },
    # The Android package is an AAR: headers/ and jni/<abi>/libonnxruntime.so.
    'onnxruntime-android.aar': {
        'url': 'https://repo1.maven.org/maven2/com/microsoft/onnxruntime/onnxruntime-android/1.30.0/'
               'onnxruntime-android-1.30.0.aar',
        'sha256': 'e7fb945e402205f6db858d65bb78d2bdb0812317b383976c9e3bceb4862c73f1',
    },
    'rtmpose-t.zip': {
        'url': 'https://download.openmmlab.com/mmpose/v1/projects/rtmposev1/onnx_sdk/'
               'rtmpose-t_simcc-body7_pt-body7_420e-256x192-026a1439_20230504.zip',
        'sha256': '937003a70832d9cc34ea16927f504792f3133e92dda1b9c626236bbbe9e805cb',
    },
    # The model's archive holds no licence; MMPose's (Apache 2.0) is its.
    'mmpose-LICENSE': {
        'url': 'https://raw.githubusercontent.com/open-mmlab/mmpose/v1.3.2/LICENSE',
        'sha256': 'aa2c6f3408169b96e5547c875deb481c91502514c1885fa0acb4b596c8909cb9',
    },
}
WINDOWS_ROOT = 'onnxruntime-win-x64-1.30.0/'
LINUX_ROOT = 'onnxruntime-linux-x64-1.30.0/'
MODEL_ROOT = '20230831/rtmpose_onnx/rtmpose-t_simcc-body7_pt-body7_420e-256x192-026a1439_20230504/'
MODEL_FILES = ('end2end.onnx', 'pipeline.json', 'deploy.json', 'detail.json')
NOTICES = ('LICENSE', 'README.md', 'ThirdPartyNotices.txt', 'Privacy.md', 'VERSION_NUMBER', 'GIT_COMMIT_ID')
ANDROID_ABIS = ('arm64-v8a', 'x86_64')


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def fetch(name, entry, verify_only):
    archive = TOOLS / name
    if archive.is_file() and digest(archive) == entry['sha256']:
        return archive
    if verify_only:
        raise SystemExit(f'missing or altered download: {archive}')
    TOOLS.mkdir(parents=True, exist_ok=True)
    print(f'fetching {entry["url"]}')
    partial = archive.with_suffix(archive.suffix + '.partial')
    with urllib.request.urlopen(entry['url']) as response, partial.open('wb') as out:
        shutil.copyfileobj(response, out)
    actual = digest(partial)
    if actual != entry['sha256']:
        partial.unlink()
        raise SystemExit(f'{name}: sha256 is {actual}, not the pinned {entry["sha256"]}')
    partial.replace(archive)
    return archive


def copy(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    with target.open('wb') as out:
        shutil.copyfileobj(source, out)


def unpack_windows(archive):
    destination = TOOLS / 'onnxruntime'
    with zipfile.ZipFile(archive) as zipped:
        for name in zipped.namelist():
            if name.endswith('/') or not name.startswith(WINDOWS_ROOT):
                continue
            relative = name[len(WINDOWS_ROOT):]
            if relative.startswith(('include/', 'lib/')) or relative in NOTICES:
                with zipped.open(name) as source:
                    copy(source, destination / relative)
    return destination


def unpack_linux(archive, destination, notices_only=False):
    with tarfile.open(archive) as packed:
        for member in packed.getmembers():
            if not member.name.startswith(LINUX_ROOT):
                continue
            relative = member.name[len(LINUX_ROOT):]
            wanted = relative in NOTICES or (not notices_only and (
                relative.startswith('include/') or
                (relative.startswith('lib/') and '.so' in relative and '/' not in relative[4:])))
            if not wanted:
                continue
            target = destination / relative
            if member.issym():
                # libonnxruntime.so -> .so.1 -> .so.1.30.0: keep the links.
                target.parent.mkdir(parents=True, exist_ok=True)
                target.unlink(missing_ok=True)
                target.symlink_to(member.linkname)
            elif member.isfile():
                with packed.extractfile(member) as source:
                    copy(source, target)
    return destination


def unpack_android(archive, linux_archive):
    destination = TOOLS / 'onnxruntime-android'
    with zipfile.ZipFile(archive) as zipped:
        for name in zipped.namelist():
            if name.startswith('headers/') and not name.endswith('/'):
                with zipped.open(name) as source:
                    copy(source, destination / 'include' / name[len('headers/'):])
        for abi in ANDROID_ABIS:
            with zipped.open('jni/%s/libonnxruntime.so' % abi) as source:
                copy(source, destination / 'lib' / abi / 'libonnxruntime.so')
    # The AAR carries no licence file; the desktop package of the same
    # release does, and the notices are the same.
    unpack_linux(linux_archive, destination, notices_only=True)
    return destination


def unpack_model(archive, licence):
    destination = TOOLS / 'rtmpose'
    destination.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as zipped:
        for name in MODEL_FILES:
            with zipped.open(MODEL_ROOT + name) as source:
                copy(source, destination / name)
    shutil.copyfile(licence, destination / 'LICENSE')
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--android', action='store_true', help='also fetch the runtime for Android')
    parser.add_argument('--verify-only', action='store_true')
    arguments = parser.parse_args()
    desktop = 'onnxruntime-win-x64.zip' if sys.platform == 'win32' else 'onnxruntime-linux-x64.tgz'
    names = [desktop, 'rtmpose-t.zip', 'mmpose-LICENSE']
    if arguments.android and 'onnxruntime-linux-x64.tgz' not in names:
        names.append('onnxruntime-linux-x64.tgz')  # for the Android runtime's licence
    if arguments.android:
        names.append('onnxruntime-android.aar')
    archives = {name: fetch(name, DOWNLOADS[name], arguments.verify_only) for name in names}
    if arguments.verify_only:
        print('downloads match their pinned digests')
        return
    if sys.platform == 'win32':
        runtime = unpack_windows(archives[desktop])
    else:
        runtime = unpack_linux(archives[desktop], TOOLS / 'onnxruntime')
    print(f'ONNX Runtime in {runtime}')
    if arguments.android:
        android = unpack_android(archives['onnxruntime-android.aar'], archives['onnxruntime-linux-x64.tgz'])
        print(f'ONNX Runtime for Android in {android}')
    model = unpack_model(archives['rtmpose-t.zip'], archives['mmpose-LICENSE'])
    print(f'RTMPose model in {model}')
    print('Configure again so CMake finds them (the pose probe and the camera motion input need them).')


if __name__ == '__main__':
    sys.exit(main())
