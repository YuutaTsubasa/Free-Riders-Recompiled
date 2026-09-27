#!/usr/bin/env python3
"""Fetch ONNX Runtime and the MediaPipe models the camera's motion input needs.

These are binary artefacts rather than repositories, so they are pinned by
SHA-256 here instead of in config/dependencies.lock.json, and unpacked into
tools/onnx (git-ignored). Windows camera releases bundle these dependencies
with their licences and notices (see docs/camera-input.md).

  python scripts/fetch_pose_model.py [--model mediapipe|rtmpose] [--platform windows|linux]
                                     [--android] [--verify-only]

The runtime is Windows x64's unless --platform linux asks for Linux x64's
(both unpack to tools/onnx/onnxruntime); --android also fetches the runtime
for Android's ABIs into tools/onnx/onnxruntime-android.
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

# ONNX Runtime is MIT; the OpenCV Zoo MediaPipe models and RTMPose are Apache 2.0.
OPENCV_ZOO_COMMIT = '47534e27c9851bb1128ccc0102f1145e27f23f98'
OPENCV_ZOO_MODELS = f'https://media.githubusercontent.com/media/opencv/opencv_zoo/{OPENCV_ZOO_COMMIT}/models'
DOWNLOADS = {
    'onnxruntime-win-x64.zip': {
        'url': 'https://github.com/microsoft/onnxruntime/releases/download/v1.30.0/onnxruntime-win-x64-1.30.0.zip',
        'sha256': 'c6ba983baf5681af108599675d2a89c2d145512d02de28aed0bff177cd0ba949',
    },
    'rtmpose-t.zip': {
        'url': 'https://download.openmmlab.com/mmpose/v1/projects/rtmposev1/onnx_sdk/'
               'rtmpose-t_simcc-body7_pt-body7_420e-256x192-026a1439_20230504.zip',
        'sha256': '937003a70832d9cc34ea16927f504792f3133e92dda1b9c626236bbbe9e805cb',
    },
    'mediapipe/pose_estimation_mediapipe_2023mar.onnx': {
        'url': OPENCV_ZOO_MODELS + '/pose_estimation_mediapipe/pose_estimation_mediapipe_2023mar.onnx',
        'sha256': '9d89c599319a18fb7d2e28451a883476164543182bafca5f09eb2cf767ed2f3f',
    },
    'mediapipe/person_detection_mediapipe_2023mar.onnx': {
        'url': OPENCV_ZOO_MODELS + '/person_detection_mediapipe/person_detection_mediapipe_2023mar.onnx',
        'sha256': '47fd5599d6fa17608f03e0eb0ae230baa6e597d7e8a2c8199fe00abea55a701f',
    },
    'mediapipe/LICENSE': {
        'url': f'https://raw.githubusercontent.com/opencv/opencv_zoo/{OPENCV_ZOO_COMMIT}/models/pose_estimation_mediapipe/LICENSE',
        'sha256': '58d1e17ffe5109a7ae296caafcadfdbe6a7d176f0bc4ab01e12a689b0499d8bd',
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
}
RUNTIME_ROOT = 'onnxruntime-win-x64-1.30.0/'
LINUX_ROOT = 'onnxruntime-linux-x64-1.30.0/'
NOTICES = ('LICENSE', 'README.md', 'ThirdPartyNotices.txt', 'Privacy.md', 'VERSION_NUMBER', 'GIT_COMMIT_ID')
ANDROID_ABIS = ('arm64-v8a', 'x86_64')
MODEL_ROOT = '20230831/rtmpose_onnx/rtmpose-t_simcc-body7_pt-body7_420e-256x192-026a1439_20230504/'
MODEL_FILES = ('end2end.onnx', 'pipeline.json', 'deploy.json', 'detail.json')


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def fetch(name, entry, verify_only):
    archive = TOOLS / name
    if archive.is_file() and digest(archive) == entry['sha256']:
        return archive
    if verify_only:
        raise SystemExit(f'missing or altered download: {archive}')
    archive.parent.mkdir(parents=True, exist_ok=True)
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


def unpack_runtime(archive):
    destination = TOOLS / 'onnxruntime'
    keep = ('include/', 'lib/')
    files = ('LICENSE', 'README.md', 'ThirdPartyNotices.txt', 'Privacy.md', 'VERSION_NUMBER', 'GIT_COMMIT_ID')
    with zipfile.ZipFile(archive) as zipped:
        for name in zipped.namelist():
            if name.endswith('/') or not name.startswith(RUNTIME_ROOT):
                continue
            relative = name[len(RUNTIME_ROOT):]
            if not relative.startswith(keep) and relative not in files:
                continue
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            with zipped.open(name) as source, target.open('wb') as out:
                shutil.copyfileobj(source, out)
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
            target.parent.mkdir(parents=True, exist_ok=True)
            if member.issym():
                # libonnxruntime.so -> .so.1 -> .so.1.30.0: keep the links.
                target.unlink(missing_ok=True)
                target.symlink_to(member.linkname)
            elif member.isfile():
                with packed.extractfile(member) as source, target.open('wb') as out:
                    shutil.copyfileobj(source, out)
    return destination


def unpack_android(archive, linux_archive):
    destination = TOOLS / 'onnxruntime-android'
    with zipfile.ZipFile(archive) as zipped:
        for name in zipped.namelist():
            if name.startswith('headers/') and not name.endswith('/'):
                target = destination / 'include' / name[len('headers/'):]
                target.parent.mkdir(parents=True, exist_ok=True)
                with zipped.open(name) as source, target.open('wb') as out:
                    shutil.copyfileobj(source, out)
        for abi in ANDROID_ABIS:
            target = destination / 'lib' / abi / 'libonnxruntime.so'
            target.parent.mkdir(parents=True, exist_ok=True)
            with zipped.open('jni/%s/libonnxruntime.so' % abi) as source, target.open('wb') as out:
                shutil.copyfileobj(source, out)
    # The AAR carries no licence file; the desktop package of the same
    # release does, and the notices are the same.
    unpack_linux(linux_archive, destination, notices_only=True)
    return destination


def unpack_model(archive):
    destination = TOOLS / 'rtmpose'
    destination.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as zipped:
        for name in MODEL_FILES:
            with zipped.open(MODEL_ROOT + name) as source, (destination / name).open('wb') as out:
                shutil.copyfileobj(source, out)
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify-only', action='store_true')
    parser.add_argument('--model', choices=('mediapipe', 'rtmpose'), default='mediapipe',
                        help='camera model family (default: mediapipe; rtmpose is legacy 2D)')
    parser.add_argument('--platform', choices=('windows', 'linux'), default='windows',
                        help='the runtime for this machine (default: windows)')
    parser.add_argument('--android', action='store_true', help="also fetch the runtime for Android's ABIs")
    arguments = parser.parse_args()
    desktop = 'onnxruntime-linux-x64.tgz' if arguments.platform == 'linux' else 'onnxruntime-win-x64.zip'
    names = [desktop]
    if arguments.android and 'onnxruntime-linux-x64.tgz' not in names:
        names.append('onnxruntime-linux-x64.tgz')  # for the Android runtime's licence
    if arguments.android:
        names.append('onnxruntime-android.aar')
    if arguments.model == 'mediapipe':
        names.extend(name for name in DOWNLOADS if name.startswith('mediapipe/'))
    else:
        names.append('rtmpose-t.zip')
    archives = {name: fetch(name, DOWNLOADS[name], arguments.verify_only) for name in names}
    if arguments.verify_only:
        print('downloads match their pinned digests')
        return
    if arguments.platform == 'linux':
        runtime = unpack_linux(archives[desktop], TOOLS / 'onnxruntime')
    else:
        runtime = unpack_runtime(archives[desktop])
    print(f'ONNX Runtime in {runtime}')
    if arguments.android:
        android = unpack_android(archives['onnxruntime-android.aar'], archives['onnxruntime-linux-x64.tgz'])
        print(f'ONNX Runtime for Android in {android}')
    if arguments.model == 'rtmpose':
        model = unpack_model(archives['rtmpose-t.zip'])
        print(f'Legacy RTMPose model in {model}')
    else:
        print(f'MediaPipe pose, detector and license in {TOOLS / "mediapipe"}')
    print('Configure again so CMake finds them (the pose probe and the camera motion input need them).')


if __name__ == '__main__':
    sys.exit(main())
