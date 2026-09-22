# Free Riders Recompiled

[繁體中文](README.zh-TW.md)

Free Riders Recompiled is an unofficial port of the Xbox 360 version of *Sonic
Free Riders* made through static recompilation, for Windows, Linux and
Android. The game's PowerPC code is translated to C++ with
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and its Xenos shaders
with [XenosRecomp](https://github.com/sonicnext-dev/XenosRecomp), then runs on
a native runtime that stands in for the console's kernel, graphics, audio,
input and Kinect.

**This project does not include any game assets. You need your own legally
acquired copy of the game (the USA/Europe disc) to play it:** the launcher
installs the game's data from your disc image. Prebuilt versions are on the
[Releases](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases)
page; to build it yourself, see [Building](docs/building.md).

> [!IMPORTANT]
> This is a work in progress. The game boots, its menus work and races can be
> played, but many parts of it are untested and it may stop or misbehave.

![The launcher](docs/images/launcher.png)

## Table of Contents

- [Status](#status)
- [System Requirements](#system-requirements)
- [How to Install](#how-to-install)
- [How to Build](#how-to-build)
- [Controls](#controls)
- [FAQ](#faq)
- [Repository Layout](#repository-layout)
- [Credits](#credits)
- [License](#license)

## Status

What works today:

- Booting from the title screen through the menus into races: Free Race from
  start to results, and Grand Prix from its story scenes into the first race.
- Direct3D 12 and Vulkan rendering, with the shaders translated as the game
  meets them; sound; saving and loading records.
- Playing without Kinect: the Kinect is emulated. Buttons stand in for the
  voice commands the menus understand, and the pad drives the body a race
  reads (leaning, jumping, kick dash, grabbing, tricks).
- A launcher that installs the game from your disc image and keeps its
  settings, in English or Traditional Chinese, on every platform.
- Linux (Vulkan, SDL2), and Android (arm64-v8a) with on-screen touch controls
  and tilt steering.

Known limits:

- Only the USA/Europe disc is supported.
- Much of the game has not been played through yet; stages and modes beyond
  those above may stop on something the runtime does not do yet.
- Linux and Android use shaders translated beforehand on Windows
  (`shaders.pack`); a shader missing from it stops the game there.
- Android has run in the emulator only; real phones are untested.

Progress notes (mostly in Traditional Chinese) are in [docs/](docs/), starting
with [docs/progress.md](docs/progress.md).

## System Requirements

- **Windows**: Windows 10 or 11 (x64), a CPU with AVX, a GPU with Direct3D 12
  (or Vulkan 1.2).
- **Linux**: x86-64 with AVX and a Vulkan 1.2 driver (tested on Ubuntu 22.04).
- **Android**: Android 9 or later, arm64-v8a, Vulkan 1.1; about 2 GB free
  for the installed game.
- Building needs the tools listed in [docs/building.md](docs/building.md).

## How to Install

1. Download the release for your system from
   [Releases](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases):
   the Windows zip, the Linux tarball, or the Android APK.
2. Unpack it into a folder of its own (on Android, install the APK).
3. Start `FreeRidersRecompiled` and choose your disc image (`.iso`) when the
   launcher asks; it copies the game's data beside itself. Then press
   **Start game**.

## How to Build

In short, on Windows:

```powershell
python scripts/bootstrap.py
./scripts/build_tools.ps1
python scripts/rom_tool.py extract --iso "your disc.iso" --output private/game --path default.xex
python scripts/prepare_recomp.py
./scripts/build_shader_translator.ps1
./scripts/build_tools.ps1 -Diagnostic
./out/build/host/FreeRidersRecompiled.exe
```

The launcher asks for your disc image on first run and installs the game
beside itself. Linux and Android builds, and what each step does, are in
[docs/building.md](docs/building.md).

## Controls

| Action | Keyboard | Controller |
| --- | --- | --- |
| Menus: move / turn the menu ring | Arrow keys | D-pad |
| Confirm (say "OK") | Z or Space | A / ✕ |
| Back | Esc, X or Backspace | B / ○ |
| Select (BACK) | Tab | BACK / Share |
| Start, pause | Enter | START |
| Race: lean | Arrow keys | Left stick |
| Crouch (hold), jump (release) | Z or Space | A |
| Kick dash | C | X |
| Brake, grab | X | B |
| Switch stance | V | Y |
| Use / shake an item | F | RT |
| Skills | Q / E | LB / RB |
| Hand cursor (Kinect-only menus) | I J K L | Right stick |

Xbox and PlayStation controllers both work. On Android, translucent touch
buttons cover the same actions when no controller is connected, and tilting
the phone steers in a race. Details: [docs/race-controls.md](docs/race-controls.md),
[docs/pad-menus.md](docs/pad-menus.md).

## FAQ

**Where are the settings and saves?** Beside the launcher: `settings.ini`,
`save/` and `game.log` (on Android, in the app's files directory). The game
plays as a local profile named "Player"; answer *Yes* when it asks "Are you
Player?" and create save data when offered.

**Why does the game need `shaders.pack` on Linux and Android?** The Xbox
shaders are translated with Windows tools while the game runs. The pack
carries the ones translated so far to machines without those tools; make it
with `python scripts/pack_shaders.py` after playing on Windows.

**Can I use the No Kinect Patch or other mods?** No mod support exists. The
Kinect emulation here is the project's own code (see [Credits](#credits)).

**Why does the release need my disc?** The release holds the recompiled
program, but none of the game's data (models, textures, sound, movies): that
comes from your own disc image. The workflow in `.github/workflows` builds and
tests only the parts that contain nothing from the game; releases are built
from a disc on the maintainer's machine ([docs/releasing.md](docs/releasing.md)).

## Repository Layout

| Path | Contents |
| --- | --- |
| `src/` | The runtime (kernel, memory, threads, files, graphics, audio, input, Kinect emulation), the launcher and the game-specific hooks |
| `scripts/` | Bootstrap, disc tools, code generation, builds and packaging |
| `config/` | Pinned dependency revisions, the supported disc's fingerprints, recompiler configuration |
| `tests/` | C++ (CTest) and Python tests |
| `android/` | The Android app's manifest, activities and resources |
| `patches/` | Patches bootstrap applies to pinned dependencies |
| `docs/` | Building, platform notes and the development record |

Not in the repository, by design (see `.gitignore`): your disc image
(`__ROM__/`), anything extracted from it (`private/`, `game/`), generated code
and build outputs (`out/`, `generated/`), downloaded dependencies (`tools/`),
saves and local reference checkouts.

## Credits

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and
  [XenosRecomp](https://github.com/sonicnext-dev/XenosRecomp): the PowerPC and
  shader recompilers.
- [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp) and
  [Marathon Recompiled](https://github.com/sonicnext-dev/MarathonRecomp): the
  ports this project learned from (runtime layout, graphics, the launcher's
  shape). None of their art is used; the launcher's art and sounds are drawn
  and synthesized by its own code.
- [Plume](https://github.com/renderbag/plume) (rendering),
  [SDL](https://www.libsdl.org) (Linux and Android), [Dear ImGui](https://github.com/ocornut/imgui)
  (launcher), [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler)
  (through [dxc-bin](https://github.com/renderbag/dxc-bin)).
- [Xenia](https://github.com/xenia-project/xenia): reference for the Xbox 360
  kernel's behaviour.
- [No Kinect Patch](https://gamebanana.com/mods/456720) by Rei-SanTH (tested
  by SmileyWorld, MagicShad and ivaschia): its reverse-engineering notes showed
  where the game reads the Kinect's voice commands and hand cursor, which
  guided the Kinect emulation here. The patch is licensed CC BY-NC-ND 4.0;
  none of its code or files are used or included.

This project was developed with the help of AI models: GPT-6 and Claude
Opus 5.

Licences and exact revisions: [THIRD_PARTY.md](THIRD_PARTY.md),
[config/dependencies.lock.json](config/dependencies.lock.json).

*Sonic Free Riders* is © SEGA. This project is not affiliated with or
endorsed by SEGA or Microsoft.

## License

The project's code is licensed under the GNU General Public License v3.0 or
later ([COPYING](COPYING)). The game's own code and data remain SEGA's and are
not covered by that grant.
