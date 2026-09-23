#!/usr/bin/env python3
"""Fetch ONNX Runtime and the RTMPose model the camera's motion input needs.

Both are release artefacts rather than repositories, so they are pinned by
SHA-256 here instead of in config/dependencies.lock.json, and unpacked into
tools/onnx (git-ignored). Neither is redistributed by this project: a release
that carries them has to carry their licences too (see docs/camera-input.md).

  python scripts/fetch_pose_model.py [--verify-only]
"""
import argparse
import hashlib
from pathlib import Path
import shutil
import sys
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
    'rtmpose-t.zip': {
        'url': 'https://download.openmmlab.com/mmpose/v1/projects/rtmposev1/onnx_sdk/'
               'rtmpose-t_simcc-body7_pt-body7_420e-256x192-026a1439_20230504.zip',
        'sha256': '937003a70832d9cc34ea16927f504792f3133e92dda1b9c626236bbbe9e805cb',
    },
}
RUNTIME_ROOT = 'onnxruntime-win-x64-1.30.0/'
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
    arguments = parser.parse_args()
    archives = {name: fetch(name, entry, arguments.verify_only) for name, entry in DOWNLOADS.items()}
    if arguments.verify_only:
        print('downloads match their pinned digests')
        return
    runtime = unpack_runtime(archives['onnxruntime-win-x64.zip'])
    model = unpack_model(archives['rtmpose-t.zip'])
    print(f'ONNX Runtime in {runtime}')
    print(f'RTMPose model in {model}')
    print('Configure again so CMake finds them (the pose probe and the camera motion input need them).')


if __name__ == '__main__':
    sys.exit(main())
