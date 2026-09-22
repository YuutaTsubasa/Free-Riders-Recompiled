#!/usr/bin/env python3
"""Packages a release from builds made on this machine.

A release carries the recompiled game (built from the maintainer's own disc,
as Unleashed Recompiled's releases do) but never the disc's data: players
install from their own disc image in the launcher. Nothing here runs in CI.

  windows  out/build/host (scripts/build_tools.ps1 -Diagnostic) and
           out/tools/shader-translator (scripts/build_shader_translator.ps1)
  linux    the build directory (scripts/build_linux.sh --diagnostic ...,
           --build DIR, default ~/sfr-build); shaders come from the pack only
  android  out/android/FreeRidersRecompiled.apk (scripts/build_android.sh)

Beside the programs go the shaders.pack collected so far
(scripts/pack_shaders.py), the licences and, on Windows, shader-tools/ (the
pinned translator, shader_common.h and dxc-bin's DXC, which the launcher hands
to the game for shaders the pack lacks: see shader_tool_environment in
src/launcher_settings.h).

Every licence file listed must exist; packaging stops otherwise.

Usage: python scripts/package_release.py windows|linux|android --version 0.1.0
       [--build DIR] [--pack out/shaders/shaders.pack] [--output out/release]
"""
import argparse
import hashlib
import io
import shutil
import sys
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DXC = ROOT / 'tools/XenosRecomp/thirdparty/dxc-bin'
# (file in the release's licenses/ directory, source)
LICENSES = [
    ('FreeRidersRecompiled-GPL-3.0.txt', ROOT / 'COPYING'),
    ('THIRD_PARTY.md', ROOT / 'THIRD_PARTY.md'),
    ('XenosRecomp-MIT.md', ROOT / 'tools/XenosRecomp/LICENSE.md'),
    ('fmt-MIT.txt', ROOT / 'tools/XenonRecomp/thirdparty/fmt/LICENSE'),
    ('Plume-MIT.txt', ROOT / 'tools/Plume/LICENSE'),
    ('DearImGui-MIT.txt', ROOT / 'tools/imgui/LICENSE.txt'),
    ('SDL-zlib.txt', ROOT / 'tools/SDL/LICENSE.txt'),
]
# Windows ships dxc-bin's DirectX Shader Compiler, which carries no licence
# file; the official texts are kept in packaging/licenses/.
DXC_LICENSES = [
    ('DirectXShaderCompiler-LLVM.txt', ROOT / 'packaging/licenses/DirectXShaderCompiler-LLVM.txt'),
    ('DirectXShaderCompiler-MS.txt', ROOT / 'packaging/licenses/DirectXShaderCompiler-MS.txt'),
]
README = """Free Riders Recompiled {version} ({platform})

An unofficial port of the Xbox 360 version of Sonic Free Riders. It contains
no game data: start the launcher and install from your own disc image
(Sonic Free Riders, USA/Europe). Source code, build instructions and the
issue tracker: https://github.com/YuutaTsubasa/Free-Riders-Recompiled

Sonic Free Riders is (c) SEGA. This project is not affiliated with or
endorsed by SEGA or Microsoft. Licences: licenses/.
"""


def need(path):
    if not path.is_file():
        sys.exit('missing %s' % path)
    return path


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def desktop_files(platform, build, pack):
    """(path in the release, source) for Windows or Linux."""
    exe = '.exe' if platform == 'windows' else ''
    files = [('FreeRidersRecompiled' + exe, need(build / ('FreeRidersRecompiled' + exe))),
             ('sfr_cpu_diagnostic' + exe, need(build / ('sfr_cpu_diagnostic' + exe)))]
    if platform == 'windows':
        files.append(('shader-tools/shader_common.h', need(ROOT / 'tools/XenosRecomp/XenosRecomp/shader_common.h')))
        files.append(('shader-tools/shader_translate.exe', need(ROOT / 'out/tools/shader-translator/shader_translate.exe')))
        for name in ('dxc.exe', 'dxcompiler.dll', 'dxil.dll'):
            files.append(('shader-tools/' + name, need(DXC / 'bin/x64' / name)))
    elif not pack:
        # No Linux translator yet (the pinned XenosRecomp corrupts memory when
        # built with clang 15 for Linux), so the pack is all it has.
        sys.exit('a Linux release needs --pack')
    if pack:
        files.append(('shaders.pack', need(pack)))
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('platform', choices=('windows', 'linux', 'android'))
    parser.add_argument('--version', required=True)
    parser.add_argument('--build', type=Path, help='desktop build directory')
    parser.add_argument('--pack', type=Path, default=ROOT / 'out/shaders/shaders.pack',
                        help='shaders.pack to include (pass "" for none)')
    parser.add_argument('--output', type=Path, default=ROOT / 'out/release')
    args = parser.parse_args()
    pack = args.pack if args.pack and str(args.pack) else None

    licenses = [('licenses/' + name, need(source))
                for name, source in LICENSES + (DXC_LICENSES if args.platform == 'windows' else [])]
    name = 'FreeRidersRecompiled-%s-%s' % (args.version, {'windows': 'windows-x64', 'linux': 'linux-x64',
                                                            'android': 'android-arm64'}[args.platform])
    args.output.mkdir(parents=True, exist_ok=True)
    readme = README.format(version=args.version, platform=args.platform).encode()

    if args.platform == 'android':
        apk = need(ROOT / 'out/android/FreeRidersRecompiled.apk')
        out = args.output / (name + '.apk')
        shutil.copyfile(apk, out)
        # The APK already holds its libraries; the licences travel beside it.
        notes = args.output / (name + '-licenses.zip')
        with zipfile.ZipFile(notes, 'w', zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('README.txt', readme)
            for inside, source in licenses:
                archive.write(source, inside)
        outputs = [out, notes]
    else:
        build = args.build or (ROOT / 'out/build/host' if args.platform == 'windows' else Path.home() / 'sfr-build')
        files = desktop_files(args.platform, build, pack) + licenses
        if args.platform == 'windows':
            out = args.output / (name + '.zip')
            with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as archive:
                archive.writestr(name + '/README.txt', readme)
                for inside, source in files:
                    archive.write(source, name + '/' + inside)
        else:
            out = args.output / (name + '.tar.gz')
            with tarfile.open(out, 'w:gz') as archive:
                info = tarfile.TarInfo(name + '/README.txt')
                info.size = len(readme)
                archive.addfile(info, io.BytesIO(readme))
                for inside, source in files:
                    member = archive.gettarinfo(str(source), name + '/' + inside)
                    executable = inside.endswith(('FreeRidersRecompiled', 'sfr_cpu_diagnostic'))
                    member.mode = 0o755 if executable else 0o644
                    with open(source, 'rb') as stream:
                        archive.addfile(member, stream)
        outputs = [out]

    for path in outputs:
        print('%s  %s  (%.1f MB)' % (sha256(path), path, path.stat().st_size / 1e6))


if __name__ == '__main__':
    main()
