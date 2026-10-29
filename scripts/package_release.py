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
  macos    run on a Mac: the build directory (--build DIR) and MoltenVK's
           macOS dylib (--moltenvk DIR, the directory holding LICENSE from
           its release); makes FreeRidersRecompiled.app, signed ad hoc
  ios      run on a Mac: the iOS build directory (cmake -DCMAKE_SYSTEM_NAME=iOS,
           --build DIR) and MoltenVK's iOS release (--moltenvk DIR); makes an
           unsigned-for-distribution .ipa (signed ad hoc) for sideloading

Beside the programs go the shaders.pack collected so far
(scripts/pack_shaders.py), the licences and, on Windows, shader-tools/ (the
pinned translator, shader_common.h and dxc-bin's DXC, which the launcher hands
to the game for shaders the pack lacks: see shader_tool_environment in
src/launcher_settings.h).

Every licence file listed must exist; packaging stops otherwise.

Usage: python scripts/package_release.py windows|linux|android|macos|ios --version 0.1.0
       [--build DIR] [--pack out/shaders/shaders.pack] [--output out/release]
"""
import argparse
import hashlib
import io
import json
import plistlib
import shutil
import subprocess
import sys
import tempfile
import tarfile
import zipfile
from pathlib import Path
from shader_pack_format import checked_shader_pack, validate_shader_pack

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
    # D3D12\D3D12Core.dll (scripts/fetch_d3d12_agility.py), on the package's
    # list of distributable files.
    ('D3D12AgilitySDK-MS.txt', ROOT / 'tools/d3d12-agility/LICENSE.txt'),
]
README = """Free Riders Recompiled {version} ({platform})

An unofficial port of the Xbox 360 version of Sonic Free Riders. It contains
no game data: start the launcher and install from your own disc image
(Sonic Free Riders, USA/Europe). Source code, build instructions and the
issue tracker: https://github.com/YuutaTsubasa/Free-Riders-Recompiled

Graphics > Resolution > Render Size changes the internal pixel resolution (360p to
1440p; native 720p by default), separately from output window size. On Windows,
Graphics > Graphics backend selects Vulkan or D3D12. New settings default to Vulkan;
existing backend choices are preserved. Restart the game after changing settings.
Keep this release's executable and shaders.pack together. See Internal-resolution.md.

VRM Avatar: choose Avatar models > Browse, then select AVATAR in
the game's character menu (it must be available in your save). Restart the
game after changing the model; Clear disables it. Camera input is optional.
One model setting is shared by both local players. See VRM-Avatar.md for
platform details and current animation/material limitations. No model is included.

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
        # The D3D12 runtime Windows 10 lacks; the game names its version.
        files.append(('D3D12/D3D12Core.dll', need(ROOT / 'tools/d3d12-agility/D3D12Core.dll')))
    elif not pack:
        # No Linux translator yet (the pinned XenosRecomp corrupts memory when
        # built with clang 15 for Linux), so the pack is all it has.
        sys.exit('a Linux or macOS release needs --pack')
    if pack:
        files.append(('shaders.pack', checked_shader_pack(need(pack))))
        for backend in ('vulkan', 'd3d12'):
            manifest = pack.parent / ('pipelines-' + backend + '.manifest')
            if not manifest.is_file():
                manifest = ROOT / 'data/pipeline-manifests' / manifest.name
            if manifest.is_file():
                files.append((manifest.name, manifest))
    return files


def apple_camera_files(root, runtime):
    """The pose models, the licences and the guide a macOS or iOS game built
    with ONNX Runtime needs (fetch_pose_model.py --apple), by path in the
    app's resources. A missing one is fatal."""
    files = [('pose/' + name, need(root / 'mediapipe' / name))
             for name in ('pose_estimation_mediapipe_2023mar.onnx', 'person_detection_mediapipe_2023mar.onnx')]
    files += [('licenses/' + inside, need(root / source)) for inside, source in (
        ('ONNXRuntime-MIT.txt', runtime + '/LICENSE'),
        ('ONNXRuntime-ThirdPartyNotices.txt', runtime + '/ThirdPartyNotices.txt'),
        ('MediaPipe-Apache-2.0.txt', 'mediapipe/LICENSE'),
    )]
    files.append(('Camera-input.md', need(ROOT / 'docs/camera-input.md')))
    return files


def drop_build_rpaths(binary):
    """The build directory's own library paths (CMake's build rpaths) have no
    place in a release, would only carry the builder's folder names, and on
    the build machine would find there what the app itself lacks."""
    commands = subprocess.run(['otool', '-l', str(binary)], check=True, capture_output=True, text=True).stdout
    for line in commands.splitlines():
        words = line.split()
        if len(words) >= 2 and words[0] == 'path' and words[1].startswith('/'):
            subprocess.run(['install_name_tool', '-delete_rpath', words[1], str(binary)], check=True)


def macos_app(app, version, build, pack, moltenvk, licenses, readme):
    """FreeRidersRecompiled.app: both programs in Contents/MacOS, MoltenVK in
    Frameworks (the game loads it from there: src/apple_vulkan_loader.h) and
    shaders.pack in Resources, which the launcher copies to Application
    Support with the player's files (src/launcher_platform_sdl.cpp)."""
    contents = app / 'Contents'
    for name in ('FreeRidersRecompiled', 'sfr_cpu_diagnostic'):
        (contents / 'MacOS').mkdir(parents=True, exist_ok=True)
        shutil.copy2(need(build / name), contents / 'MacOS' / name)
    dylib = need(moltenvk / 'MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib')
    (contents / 'Frameworks').mkdir()
    shutil.copy2(dylib, contents / 'Frameworks/libMoltenVK.dylib')
    resources = contents / 'Resources'
    resources.mkdir()
    licenses = licenses + [('licenses/MoltenVK-Apache-2.0.txt', need(moltenvk / 'LICENSE'))]
    # Camera motion (#305) when the game was built with ONNX Runtime: the
    # dylib beside MoltenVK, where the game's rpath looks, and the models.
    if b'libonnxruntime' in (contents / 'MacOS/sfr_cpu_diagnostic').read_bytes():
        shutil.copy2(need(ROOT / 'tools/onnx/onnxruntime-macos/lib/libonnxruntime.1.dylib'),
                     contents / 'Frameworks/libonnxruntime.1.dylib')
        licenses = licenses + apple_camera_files(ROOT / 'tools/onnx', 'onnxruntime-macos')
        print('camera motion: included')
    else:
        print('camera motion: left out (no ONNX Runtime in the build)')
    for name in ('FreeRidersRecompiled', 'sfr_cpu_diagnostic'):
        drop_build_rpaths(contents / 'MacOS' / name)
    for inside, source in desktop_files('macos', build, pack)[2:] + licenses:
        (resources / inside).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, resources / inside)
    (resources / 'README.txt').write_bytes(readme)
    # The icon, at the sizes iconutil wants, from the 256 px PNG.
    iconset = Path(tempfile.mkdtemp()) / 'AppIcon.iconset'
    iconset.mkdir()
    for size in (16, 32, 64, 128, 256):
        for scale, suffix in ((1, ''), (2, '@2x')):
            if size * scale <= 256:
                subprocess.run(['sips', '-z', str(size * scale), str(size * scale), str(ROOT / 'packaging/icon/app.png'),
                                '--out', str(iconset / f'icon_{size}x{size}{suffix}.png')],
                               check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['iconutil', '-c', 'icns', str(iconset), '-o', str(resources / 'AppIcon.icns')], check=True)
    with open(contents / 'Info.plist', 'wb') as stream:
        plistlib.dump({
            'CFBundleName': 'Free Riders Recompiled',
            'CFBundleDisplayName': 'Free Riders Recompiled',
            'CFBundleIdentifier': 'com.freeriders.recompiled',
            'CFBundleExecutable': 'FreeRidersRecompiled',
            'CFBundleIconFile': 'AppIcon',
            'CFBundlePackageType': 'APPL',
            'CFBundleShortVersionString': version,
            'CFBundleVersion': version,
            'CFBundleInfoDictionaryVersion': '6.0',
            'LSMinimumSystemVersion': '14.0',  # ONNX Runtime 1.30's dylib (#305)
            'LSApplicationCategoryType': 'public.app-category.games',
            'NSHighResolutionCapable': True,
            'NSCameraUsageDescription': 'Camera input lets you ride with your body, as with Kinect.',
            'NSMicrophoneUsageDescription': 'Voice commands, as with Kinect.',
        }, stream)
    # Not notarised: signed ad hoc, which Apple Silicon requires to run at all.
    subprocess.run(['codesign', '--force', '--deep', '--sign', '-', str(app)], check=True)


# What the iOS game asks of the system: a guest address space of several
# gigabytes and more memory than an app gets by default. Sideloading tools
# that re-sign the app keep these only where the signing identity may.
IOS_ENTITLEMENTS = {
    'com.apple.developer.kernel.extended-virtual-addressing': True,
    'com.apple.developer.kernel.increased-memory-limit': True,
}


# The iOS icon's sizes in points with their scales, by device (Xcode's list):
# the home screen, Spotlight, Settings and notifications, and the 1024 px one
# the system and sideloading tools show.
IOS_ICONS = [
    ('iphone', '20x20', ('2x', '3x')), ('iphone', '29x29', ('2x', '3x')), ('iphone', '40x40', ('2x', '3x')),
    ('iphone', '60x60', ('2x', '3x')),
    ('ipad', '20x20', ('1x', '2x')), ('ipad', '29x29', ('1x', '2x')), ('ipad', '40x40', ('1x', '2x')),
    ('ipad', '76x76', ('1x', '2x')), ('ipad', '83.5x83.5', ('2x',)),
    ('ios-marketing', '1024x1024', ('1x',)),
]


def ios_icon(app):
    """The app's icon (Issue #185: there was none, so the home screen showed
    a blank one): an asset catalog of the PNG the other platforms use, compiled
    by actool into the app as Assets.car and the PNGs older readers look for.
    Returns the Info.plist keys that name it."""
    work = Path(tempfile.mkdtemp())
    catalog = work / 'Assets.xcassets'
    icons = catalog / 'AppIcon.appiconset'
    icons.mkdir(parents=True)
    (catalog / 'Contents.json').write_text(json.dumps({'info': {'author': 'xcode', 'version': 1}}))
    images = []
    for idiom, size, scales in IOS_ICONS:
        for scale in scales:
            pixels = round(float(size.split('x')[0]) * int(scale[0]))
            name = f'icon-{pixels}.png'
            if not (icons / name).exists():
                subprocess.run(['sips', '-z', str(pixels), str(pixels), str(ROOT / 'packaging/icon/app.png'),
                                '--out', str(icons / name)], check=True, stdout=subprocess.DEVNULL)
            images.append({'filename': name, 'idiom': idiom, 'scale': scale, 'size': size})
    (icons / 'Contents.json').write_text(json.dumps({'images': images, 'info': {'author': 'xcode', 'version': 1}}))
    partial = work / 'icon-info.plist'
    subprocess.run(['xcrun', 'actool', str(catalog), '--compile', str(app), '--platform', 'iphoneos',
                    '--minimum-deployment-target', '16.3', '--app-icon', 'AppIcon',
                    '--target-device', 'iphone', '--target-device', 'ipad',
                    '--output-partial-info-plist', str(partial), '--errors', '--warnings'],
                   check=True, stdout=subprocess.DEVNULL)
    with open(partial, 'rb') as stream:
        keys = plistlib.load(stream)
    if 'CFBundleIcons' not in keys:
        sys.exit('actool named no app icon')
    return keys


def ios_app(app, version, build, pack, moltenvk, licenses, readme):
    """FreeRidersRecompiled.app for iOS: the launcher is the app's program,
    the game a framework it loads at Play (src/ios_game_entry.cpp), SDL and
    MoltenVK beside it in Frameworks, and the shaders in the bundle, which
    the launcher copies to the app's Documents (src/launcher_platform_sdl.cpp)."""
    frameworks = app / 'Frameworks'
    game = frameworks / 'FreeRidersGame.framework'
    game.mkdir(parents=True)
    shutil.copy2(need(build / 'FreeRidersRecompiled.app/FreeRidersRecompiled'), app / 'FreeRidersRecompiled')
    shutil.copy2(need(build / 'libFreeRidersGame.dylib'), game / 'FreeRidersGame')
    with open(game / 'Info.plist', 'wb') as stream:
        plistlib.dump({'CFBundleExecutable': 'FreeRidersGame', 'CFBundleIdentifier': 'com.freeriders.recompiled.game',
                       'CFBundleName': 'FreeRidersGame', 'CFBundlePackageType': 'FMWK',
                       'CFBundleShortVersionString': version, 'CFBundleVersion': version,
                       'MinimumOSVersion': '16.3'}, stream)
    shutil.copy2(need(build / 'tools/SDL/libSDL2-2.0.0.dylib'), frameworks / 'libSDL2-2.0.0.dylib')
    # MoltenVK's iOS release is a framework; the game loads it as the macOS
    # app does, by the dylib name volk asks for (src/apple_vulkan_loader.h).
    shutil.copy2(need(moltenvk / 'MoltenVK/dynamic/MoltenVK.xcframework/ios-arm64/MoltenVK.framework/MoltenVK'),
                 frameworks / 'libMoltenVK.dylib')
    subprocess.run(['install_name_tool', '-id', '@rpath/libMoltenVK.dylib', str(frameworks / 'libMoltenVK.dylib')],
                   check=True)
    licenses = licenses + [('licenses/MoltenVK-Apache-2.0.txt', need(moltenvk / 'LICENSE'))]
    if not pack:
        sys.exit('an iOS release needs --pack: iOS cannot translate shaders')
    files = [('shaders.pack', checked_shader_pack(need(pack)))]
    manifest = pack.parent / 'pipelines-vulkan.manifest'
    if not manifest.is_file():
        manifest = ROOT / 'data/pipeline-manifests' / manifest.name
    if manifest.is_file():
        files.append((manifest.name, manifest))
    # Camera motion (#305): ONNX Runtime is linked into the game itself.
    if b'OrtGetApiBase' in (game / 'FreeRidersGame').read_bytes():
        files += apple_camera_files(ROOT / 'tools/onnx', 'onnxruntime-ios')
        print('camera motion: included')
    else:
        print('camera motion: left out (no ONNX Runtime in the build)')
    for inside, source in files + licenses:
        (app / inside).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, app / inside)
    (app / 'README.txt').write_bytes(readme)
    icon_keys = ios_icon(app)
    with open(app / 'Info.plist', 'wb') as stream:
        plistlib.dump({
            **icon_keys,
            'CFBundleName': 'Free Riders Recompiled',
            'CFBundleDisplayName': 'Free Riders',
            'CFBundleIdentifier': 'com.freeriders.recompiled',
            'CFBundleExecutable': 'FreeRidersRecompiled',
            'CFBundlePackageType': 'APPL',
            'CFBundleShortVersionString': version,
            'CFBundleVersion': version,
            'CFBundleInfoDictionaryVersion': '6.0',
            'CFBundleSupportedPlatforms': ['iPhoneOS'],
            'MinimumOSVersion': '16.3',
            'UIDeviceFamily': [1, 2],
            'UIRequiredDeviceCapabilities': ['arm64', 'metal'],
            'UISupportedInterfaceOrientations': ['UIInterfaceOrientationLandscapeLeft',
                                                 'UIInterfaceOrientationLandscapeRight'],
            'UISupportedInterfaceOrientations~ipad': ['UIInterfaceOrientationLandscapeLeft',
                                                      'UIInterfaceOrientationLandscapeRight'],
            # Without a launch screen iOS runs the app at an old phone's size.
            'UILaunchScreen': {},
            'UIRequiresFullScreen': True,
            'UIStatusBarHidden': True,
            # The Files app shows the app's Documents: the disc image can be
            # put there, and settings, saves and game.log are there.
            'UIFileSharingEnabled': True,
            'LSSupportsOpeningDocumentsInPlace': True,
            'UIApplicationSupportsIndirectInputEvents': True,  # a mouse or trackpad
            'GCSupportsControllerUserInteraction': True,
            # iOS ends an app that opens the camera without saying why.
            'NSCameraUsageDescription': 'Camera input lets you ride with your body, as with Kinect.',
            'GCSupportedGameControllers': [{'ProfileName': 'ExtendedGamepad'}],
        }, stream)
    for binary in (app / 'FreeRidersRecompiled', game / 'FreeRidersGame'):
        drop_build_rpaths(binary)
    entitlements = Path(tempfile.mkdtemp()) / 'entitlements.plist'
    with open(entitlements, 'wb') as stream:
        plistlib.dump(IOS_ENTITLEMENTS, stream)
    # Inside out: each library, then the framework, then the app with its
    # entitlements. Ad hoc: sideloading tools sign it again with the player's
    # own identity.
    for library in (frameworks / 'libSDL2-2.0.0.dylib', frameworks / 'libMoltenVK.dylib', game):
        subprocess.run(['codesign', '--force', '--sign', '-', str(library)], check=True)
    subprocess.run(['codesign', '--force', '--sign', '-', '--entitlements', str(entitlements), str(app)], check=True)


def camera_files(root, platform, game):
    """Windows or Linux MediaPipe bundle. Missing runtime, model or licence is fatal."""
    files = []
    if platform == 'windows':
        for name in ('onnxruntime.dll', 'onnxruntime_providers_shared.dll'):
            files.append((name, need(root / 'onnxruntime/lib' / name)))
    else:
        # A game built without the runtime would carry the models for nothing.
        if b'libonnxruntime.so.1' not in game.read_bytes():
            sys.exit('%s was built without ONNX Runtime (fetch_pose_model.py --platform linux, then configure again)' % game)
        # The game's RUNPATH is $ORIGIN and it asks for the .so.1 name; the
        # tarball holds the file itself under that name, not the links.
        files.append(('libonnxruntime.so.1', need(root / 'onnxruntime/lib/libonnxruntime.so.1.30.0')))
        files.append(('libonnxruntime_providers_shared.so', need(root / 'onnxruntime/lib/libonnxruntime_providers_shared.so')))
    for name in ('pose_estimation_mediapipe_2023mar.onnx', 'person_detection_mediapipe_2023mar.onnx'):
        files.append(('pose/' + name, need(root / 'mediapipe' / name)))
    for inside, source in (
        ('ONNXRuntime-MIT.txt', 'onnxruntime/LICENSE'),
        ('ONNXRuntime-ThirdPartyNotices.txt', 'onnxruntime/ThirdPartyNotices.txt'),
        ('MediaPipe-Apache-2.0.txt', 'mediapipe/LICENSE'),
    ):
        files.append(('licenses/' + inside, need(root / source)))
    return files


def android_camera_licenses(root):
    """The licences for the ONNX Runtime and models an APK carries."""
    return [('licenses/' + inside, need(root / source)) for inside, source in (
        ('ONNXRuntime-MIT.txt', 'onnxruntime-android/LICENSE'),
        ('ONNXRuntime-ThirdPartyNotices.txt', 'onnxruntime-android/ThirdPartyNotices.txt'),
        ('MediaPipe-Apache-2.0.txt', 'mediapipe/LICENSE'),
    )]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('platform', choices=('windows', 'linux', 'android', 'macos', 'ios'))
    parser.add_argument('--version', required=True)
    parser.add_argument('--build', type=Path, help='desktop build directory')
    parser.add_argument('--pack', type=Path, default=ROOT / 'out/shaders/shaders.pack',
                        help='shaders.pack to include (pass "" for none)')
    parser.add_argument('--output', type=Path, default=ROOT / 'out/release')
    parser.add_argument('--moltenvk', type=Path, help='macOS: the unpacked MoltenVK release directory')
    parser.add_argument('--camera', action='store_true', help='bundle the MediaPipe models and ONNX Runtime (Windows, Linux)')
    args = parser.parse_args()
    if args.camera and args.platform not in ('windows', 'linux'):
        parser.error('--camera packages the Windows and Linux runtimes (an APK carries its own)')
    pack = args.pack if args.pack and str(args.pack) else None

    licenses = [('licenses/' + name, need(source))
                for name, source in LICENSES + (DXC_LICENSES if args.platform == 'windows' else [])]
    name = 'FreeRidersRecompiled-%s-%s' % (args.version, {'windows': 'windows-x64', 'linux': 'linux-x64',
                                                            'android': 'android-arm64',
                                                            'macos': 'macos-arm64',
                                                            'ios': 'ios-arm64'}[args.platform])
    args.output.mkdir(parents=True, exist_ok=True)
    readme = README.format(version=args.version, platform=args.platform).encode()

    if args.platform == 'ios':
        if sys.platform != 'darwin' or not args.moltenvk or not args.build:
            parser.error('ios packages on a Mac, with --build and --moltenvk')
        staging = Path(tempfile.mkdtemp())
        ios_app(staging / 'Payload/FreeRidersRecompiled.app', args.version, args.build, pack, args.moltenvk,
                licenses, readme)
        out = args.output / (name + '.ipa')
        out.unlink(missing_ok=True)
        # An .ipa is a zip with Payload/ at its root; ditto keeps the signatures.
        subprocess.run(['ditto', '-c', '-k', '--keepParent', str(staging / 'Payload'), str(out)], check=True)
        shutil.rmtree(staging)
        outputs = [out]
    elif args.platform == 'macos':
        if sys.platform != 'darwin' or not args.moltenvk or not args.build:
            parser.error('macos packages on a Mac, with --build and --moltenvk')
        staging = Path(tempfile.mkdtemp())
        app = staging / name / 'FreeRidersRecompiled.app'
        macos_app(app, args.version, args.build, pack, args.moltenvk, licenses, readme)
        (staging / name / 'README.txt').write_bytes(readme)
        for inside, source in (('VRM-Avatar.md', 'docs/vrm-avatar.md'),
                               ('Internal-resolution.md', 'docs/internal-resolution.md')):
            shutil.copyfile(need(ROOT / source), staging / name / inside)
        out = args.output / (name + '.zip')
        out.unlink(missing_ok=True)
        # ditto keeps the bundle's signature, symlinks and modes intact.
        subprocess.run(['ditto', '-c', '-k', '--keepParent', str(staging / name), str(out)], check=True)
        shutil.rmtree(staging)
        outputs = [out]
    elif args.platform == 'android':
        apk = need(ROOT / 'out/android/FreeRidersRecompiled.apk')
        with zipfile.ZipFile(apk) as archive:
            apk_names = archive.namelist()
            try:
                validate_shader_pack(archive.read('assets/shaders.pack'))
            except (KeyError, ValueError) as error:
                sys.exit(f'Android shader pack: {error}')
        out = args.output / (name + '.apk')
        shutil.copyfile(apk, out)
        # The APK already holds its libraries; the licences travel beside it.
        notes = args.output / (name + '-licenses.zip')
        with zipfile.ZipFile(notes, 'w', zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('README.txt', readme)
            archive.write(need(ROOT / 'docs/vrm-avatar.md'), 'VRM-Avatar.md')
            archive.write(need(ROOT / 'docs/internal-resolution.md'), 'Internal-resolution.md')
            for inside, source in licenses:
                archive.write(source, inside)
            # build_android.sh carries the pose models and ONNX Runtime only
            # when both were fetched (fetch_pose_model.py --android).
            if any(n.endswith('/libonnxruntime.so') for n in apk_names):
                for inside, source in android_camera_licenses(ROOT / 'tools/onnx'):
                    archive.write(source, inside)
                archive.write(need(ROOT / 'docs/camera-input.md'), 'Camera-input.md')
        outputs = [out, notes]
    else:
        build = args.build or (ROOT / 'out/build/host' if args.platform == 'windows' else Path.home() / 'sfr-build')
        files = desktop_files(args.platform, build, pack) + licenses
        files.append(('VRM-Avatar.md', need(ROOT / 'docs/vrm-avatar.md')))
        files.append(('Internal-resolution.md', need(ROOT / 'docs/internal-resolution.md')))
        files.append(('Pipeline-preparation.md', need(ROOT / 'docs/pipeline-preparation.md')))
        if args.camera:
            exe = '.exe' if args.platform == 'windows' else ''
            # sfr_cpu_diagnostic is the game; FreeRidersRecompiled is the launcher.
            files += camera_files(ROOT / 'tools/onnx', args.platform, build / ('sfr_cpu_diagnostic' + exe))
            files.append(('Camera-input.md', need(ROOT / 'docs/camera-input.md')))
            readme += b'\nCamera motion: enable Camera > Motion in the launcher. Models and runtime are included.\nSkeleton debug window is optional. Camera controls 1P; controller input takes priority.\n'
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
                    executable = inside.endswith(('FreeRidersRecompiled', 'sfr_cpu_diagnostic', '.so', '.so.1'))
                    member.mode = 0o755 if executable else 0o644
                    with open(source, 'rb') as stream:
                        archive.addfile(member, stream)
        outputs = [out]

    for path in outputs:
        print('%s  %s  (%.1f MB)' % (sha256(path), path, path.stat().st_size / 1e6))


if __name__ == '__main__':
    main()
