"""Packages the Android APK from the prebuilt native libraries without Gradle.

scripts/build_android.sh builds libmain.so and libSDL2.so into
android/app/src/main/jniLibs/<abi>/ and then runs this. Only the installed
Android SDK (build-tools, a platform) and a JDK are used, so nothing is
downloaded. The APK is signed with the local debug key (~/.android/debug.keystore,
created with the standard debug-key settings when missing).

Usage: python scripts/package_android.py [--output out/android/FreeRidersRecompiled.apk] [--abi ABI]... [--pack FILE]
"""
import argparse
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROJECT = ROOT / 'android' / 'app' / 'src' / 'main'
SDL_JAVA = ROOT / 'tools' / 'SDL' / 'android-project' / 'app' / 'src' / 'main' / 'java'
MIN_SDK, TARGET_SDK = 28, 35
VERSION_CODE, VERSION_NAME = 2, '0.1.1'


def version_key(path):
    return [int(part) if part.isdigit() else 0 for part in path.name.replace('-', '.').split('.')]


def sdk_root():
    for name in ('ANDROID_HOME', 'ANDROID_SDK_ROOT'):
        if os.environ.get(name):
            return Path(os.environ[name])
    for candidate in (Path.home() / 'AppData/Local/Android/Sdk', Path.home() / 'Android/Sdk'):
        if candidate.is_dir():
            return candidate
    sys.exit('No Android SDK: set ANDROID_HOME')


def tool(directory, name):
    for suffix in ('', '.exe', '.bat'):
        path = directory / (name + suffix)
        if path.exists():
            return str(path)
    sys.exit('missing %s in %s' % (name, directory))


def jdk_tool(name):
    home = os.environ.get('JAVA_HOME')
    if home:
        return tool(Path(home) / 'bin', name)
    found = shutil.which(name)
    if not found:
        sys.exit('No JDK: set JAVA_HOME')
    return found


def run(*command):
    subprocess.run([str(part) for part in command], check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', default=str(ROOT / 'out' / 'android' / 'FreeRidersRecompiled.apk'))
    parser.add_argument('--abi', action='append', help='only these ABIs (default: every built one)')
    parser.add_argument('--pack', help='a shaders.pack to carry as an asset (LauncherActivity copies it out)')
    args = parser.parse_args()

    sdk = sdk_root()
    build_tools = max((sdk / 'build-tools').iterdir(), key=version_key)
    platform = max((p for p in (sdk / 'platforms').iterdir() if (p / 'android.jar').exists()), key=version_key)
    android_jar = platform / 'android.jar'
    libraries = PROJECT / 'jniLibs'
    abis = sorted(p.name for p in libraries.iterdir() if (p / 'libmain.so').exists()) if libraries.is_dir() else []
    if args.abi:
        abis = [abi for abi in abis if abi in args.abi]
    if not abis:
        sys.exit('No native libraries: run scripts/build_android.sh first')
    if not SDL_JAVA.is_dir():
        sys.exit('Run scripts/bootstrap.py to obtain the pinned SDL (tools/SDL)')

    work = ROOT / 'out' / 'build' / 'android-apk'
    shutil.rmtree(work, ignore_errors=True)
    (work / 'gen').mkdir(parents=True)
    (work / 'classes').mkdir()
    (work / 'dex').mkdir()

    aapt2 = tool(build_tools, 'aapt2')
    run(aapt2, 'compile', '--dir', PROJECT / 'res', '-o', work / 'resources.zip')
    run(aapt2, 'link', '-o', work / 'base.apk', '-I', android_jar, '--manifest', PROJECT / 'AndroidManifest.xml',
        '--java', work / 'gen', '--min-sdk-version', MIN_SDK, '--target-sdk-version', TARGET_SDK,
        '--version-code', VERSION_CODE, '--version-name', VERSION_NAME,
        '--auto-add-overlay', work / 'resources.zip')

    sources = [str(p) for p in SDL_JAVA.rglob('*.java')] + [str(p) for p in (PROJECT / 'java').rglob('*.java')] \
        + [str(p) for p in (work / 'gen').rglob('*.java')]
    run(jdk_tool('javac'), '-nowarn', '-Xlint:-options', '-source', '11', '-target', '11', '-encoding', 'UTF-8',
        '-classpath', android_jar, '-d', work / 'classes', *sources)
    classes = [str(p) for p in (work / 'classes').rglob('*.class')]
    run(tool(build_tools, 'd8'), '--release', '--min-api', MIN_SDK, '--lib', android_jar,
        '--output', work / 'dex', *classes)

    unsigned = work / 'unsigned.apk'
    shutil.copy(work / 'base.apk', unsigned)
    with zipfile.ZipFile(unsigned, 'a') as apk:
        apk.write(work / 'dex' / 'classes.dex', 'classes.dex', compress_type=zipfile.ZIP_DEFLATED)
        # Uncompressed so the loader maps them straight from the APK.
        for abi in abis:
            for library in sorted((libraries / abi).glob('*.so')):
                apk.write(library, 'lib/%s/%s' % (abi, library.name), compress_type=zipfile.ZIP_STORED)
        if args.pack:
            apk.write(args.pack, 'assets/shaders.pack', compress_type=zipfile.ZIP_DEFLATED)
    aligned = work / 'aligned.apk'
    run(tool(build_tools, 'zipalign'), '-f', '-P', '16', '4', unsigned, aligned)

    keystore = Path.home() / '.android' / 'debug.keystore'
    if not keystore.exists():
        keystore.parent.mkdir(parents=True, exist_ok=True)
        run(jdk_tool('keytool'), '-genkeypair', '-keystore', keystore, '-storepass', 'android', '-keypass', 'android',
            '-alias', 'androiddebugkey', '-dname', 'CN=Android Debug,O=Android,C=US', '-keyalg', 'RSA',
            '-keysize', '2048', '-validity', '10000')
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    run(tool(build_tools, 'apksigner'), 'sign', '--ks', keystore, '--ks-pass', 'pass:android',
        '--key-pass', 'pass:android', '--ks-key-alias', 'androiddebugkey', '--out', output, aligned)
    print('APK: %s (%s)' % (output, ', '.join(abis)))


if __name__ == '__main__':
    main()
