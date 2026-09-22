# Building

This is how to build Free Riders Recompiled from source; players can use the
[releases](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases)
instead. The game's code is C++ generated from the `default.xex` on **your
own** Sonic Free Riders disc. The disc's files and everything extracted from
them stay on your machine (`private/`, `game/` and `out/` are ignored by Git);
nothing in this repository contains game code or data. Making a release from
such a build is described in [releasing.md](releasing.md).

Supported disc: *Sonic Free Riders (USA, Europe) (En,Ja,Fr,De,Es,It)*. The
scripts check its `default.xex` fingerprint (`config/source.json`) and refuse
anything else.

## 1. Tools

| Platform | Needed |
| --- | --- |
| Windows (build and game-code generation) | Git, Python 3.12+, Visual Studio 2022 with the C++ desktop workload, LLVM/Clang (`C:\Program Files\LLVM`), CMake 3.20+, Ninja |
| Linux | Git, Python 3.11+, Clang 15+, CMake, Ninja, `libsdl2-dev`, `libx11-dev`, a Vulkan driver (`mesa-vulkan-drivers` works) |
| Android (built on Windows or Linux) | Everything above, plus the Android SDK (build-tools, platform 35), NDK 27+ and JDK 17 |

Fetch the pinned third-party sources (XenonRecomp, XenosRecomp with its
prebuilt DXC, Plume, Dear ImGui, SDL) into `tools/`:

```bash
python scripts/bootstrap.py
```

`config/dependencies.lock.json` records every revision. Bootstrap never
changes an existing checkout, and applies (and afterwards verifies) the small
Plume patch in `patches/`.

## 2. Build the tools

Windows (PowerShell, from the repository root):

```powershell
./scripts/build_tools.ps1
```

This builds XenonRecomp/XenonAnalyse, the image dumper, the launcher
(`out/build/host/FreeRidersRecompiled.exe`) and the runtime's tests
(`ctest --test-dir out/build/host`).

## 3. Generate the game code

Extract `default.xex` from your disc image, then generate the code:

```powershell
python scripts/rom_tool.py extract --iso "path/to/Sonic Free Riders (USA, Europe) (En,Ja,Fr,De,Es,It).iso" --output private/game --path default.xex
python scripts/prepare_recomp.py
```

`prepare_recomp.py` decodes the executable, analyses its jump tables, runs
XenonRecomp and turns its output into the checked form the runtime builds
(`out/recomp/diagnostic`, about six minutes). It refuses to overwrite an
existing output; pass `--output DIR` for another one. The ISO and `private/`
stay on your machine (both are ignored by Git).

## 4. Build the game

### Windows

```powershell
./scripts/build_shader_translator.ps1
./scripts/build_tools.ps1 -Diagnostic
./out/build/host/FreeRidersRecompiled.exe
```

The launcher installs the game on first run: choose your disc image (or a
folder with its files); it copies the disc's files to `game/assets` beside the
launcher and decodes the executable to `game/image`. Then press **Start game**.
Settings are kept in `settings.ini`, saves in `save/`, the game's trace in
`game.log`, all beside the launcher.

The shader translator lets the game translate the Xbox shaders it meets while
running (Direct3D 12 by default, Vulkan in the launcher's Advanced tab).
`python scripts/pack_shaders.py` collects the shaders translated so far into
`out/shaders/shaders.pack` for machines without the translator (Linux,
Android).

### Linux

```bash
scripts/build_linux.sh --diagnostic out/recomp/diagnostic
~/sfr-build/FreeRidersRecompiled
```

The build directory defaults to `~/sfr-build` (`SFR_LINUX_BUILD` changes it).
The launcher is the same as on Windows; it uses zenity or kdialog for file
dialogs when installed. Linux has no shader translator yet, so copy a
`shaders.pack` made on Windows next to the launcher (the Game files tab can
copy one in). Game-code generation (step 3) has so far been run on Windows;
the generated directory can be copied to Linux. Details: [linux.md](linux.md).

### Android

```bash
scripts/build_android.sh --diagnostic out/recomp/diagnostic
```

This builds `arm64-v8a` and `x86_64` with the NDK and packages
`out/android/FreeRidersRecompiled.apk` with the SDK's own tools (no Gradle, no
downloads). Install it, copy your disc image and a `shaders.pack` to the phone,
then install from the app's launcher. Hold Back to leave the game. Details and
limits: [android.md](android.md).

## Tests

```bash
ctest --test-dir out/build/host                              # Windows
xvfb-run -a ctest --test-dir ~/sfr-build                     # Linux (or with a display)
python -m unittest discover -s tests
```

Tests that need the game's files skip unless their environment variables point
at them (see `tests/`). The GitHub workflow (`.github/workflows/build.yml`)
builds and tests everything that does not need the game on Windows and Linux,
and the Android launcher libraries.
