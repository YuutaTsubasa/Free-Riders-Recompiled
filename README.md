# Free Riders Recompiled

[繁體中文](README.zh-TW.md)

Free Riders Recompiled is an unofficial port of the Xbox 360 version of *Sonic
Free Riders* for Windows, Linux and Android, built through static recompilation.
Play with a controller or keyboard, use **Kinect / Kinect v2 or webcam motion
controls on Windows**, and bring your own **VRM model** to the in-game AVATAR
rider. Kinect v2 support is experimental and still needs hardware validation.

The game's PowerPC code is translated to C++ with
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
- [Release notes](#release-notes)
- [System Requirements](#system-requirements)
- [How to Install](#how-to-install)
- [Settings](#settings)
- [How to Build](#how-to-build)
- [Controls](#controls)
- [Kinect and Kinect v2](#kinect-and-kinect-v2)
- [Camera motion input](#camera-motion-input)
- [VRM Avatar models](#vrm-avatar-models)
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
- Physical Kinect and Kinect v2 input on Windows, with sensor skeletons
  passed to the game. Xbox 360 Kinect has been tested through races;
  Kinect v2 support is experimental. See [setup and requirements](#kinect-and-kinect-v2).
- A launcher that installs the game from your disc image and keeps its
  settings, in English or Traditional Chinese, on every platform.
- Linux (Vulkan, SDL2), and Android (arm64-v8a) with on-screen touch controls
  and tilt steering.
- Optional webcam motion input for 1P on Windows, with estimated 3D body
  joints, controller handoff and a separate skeleton debug window.
- Experimental custom VRM Avatar models, selected in the launcher, with
  game-driven animation and hand-held items.
- File-replacement mods in HedgeModManager's format, managed in the
  launcher's Mods tab ([Mods](docs/mods.md)).

Known limits:

- Only the USA/Europe disc is supported.
- Much of the game has not been played through yet; stages and modes beyond
  those above may stop on something the runtime does not do yet.
- Linux and Android use shaders translated beforehand on Windows
  (`shaders.pack`, included in the releases). The game creates all of its
  shaders while it boots; releases include the matching shader pack.
- Android has been tried in the emulator, on Pocket S2 Pro (Adreno 750),
  and on AYN Thor (Adreno 740). The latest Android 10+ build completed a
  race on Thor at approximately 23–24 FPS with race time matching real time.
  Intro movies can still play slowly; this does not imply 60 FPS or the
  same performance on other devices. See [Android performance](docs/android-performance.md).

Progress notes (mostly in Traditional Chinese) are in [docs/](docs/), starting
with [docs/progress.md](docs/progress.md).

## Release notes

- [v0.1.0 — First playable preview](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.1.0)
  Released Windows, Linux and Android builds, with playable menus and races, save support, and controller/keyboard gameplay without Kinect. Included an English/Traditional Chinese launcher and Android touch controls and tilt steering.
- [v0.1.1 — Early stability and rendering fixes](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.1.1)
  Fixed a Grand Prix loading failure, removed accidentally enabled profiling overhead, and corrected rendering at non-native window sizes. Improved Android texture compatibility and surface recovery, and bundled a complete shader pack.
- [v0.1.2 — Race results fix](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.1.2)
  Fixed the game stopping at race results when it attempted to award an achievement. Achievement requests are now handled locally without Xbox Live.
- [v0.1.3 — Customizable controls](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.1.3)
  Added desktop settings for player input sources, controller selection and keyboard/gamepad bindings. Improved Vulkan compatibility with display surfaces that lack certain capabilities.
- [v0.1.4 — Crouch and jump fixes](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.1.4)
  Fixed controller input conflicts that could cancel crouch charging or jump release, including when holding the left stick down.
- [v0.1.5 — Local two-player support](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.1.5)
  Added independent player input routing and split-screen corrections. Fixed Gear selection getting stuck after Player 1 confirmed, reduced repetitive input logging, and removed unnecessary CPU queries.
- [v0.2.0 — Webcam motion controls](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.2.0)
  Added optional Windows webcam controls using estimated 3D body movement, a skeleton debug window, and automatic controller/camera handoff. Improved motion gestures and fixed single-player viewport stretching and exposure when 2P was disabled.
- [v0.2.1 — Shader and Windows startup fixes](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.2.1)
  Corrected the incompatible shader pack shipped with v0.2.0, which could cause a white screen or Vulkan device loss. Added shader-pack validation and bundled the D3D12 Agility runtime for affected Windows 10 systems.
- [v0.3.0 — Custom VRM avatars](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.3.0)
  Added experimental VRM 0.x/1.0 models for the in-game AVATAR rider, with game-driven animation, stance changes and held-item positioning. Models can be selected through the launcher.
- [v0.4.0 — Real Kinect support](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.4.0)
  Added physical Kinect input on Windows, sensor previews and Kinect v1 tilt controls. Improved player tracking, webcam handling and Windows graphics compatibility. Xbox 360 Kinect has been tested in races; Kinect v2 remains experimental.
- [v0.4.1 — Rendering options and performance work](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.4.1)
  Fixed shader reload failures during scene changes and controller-related rider pose distortion. Added independent internal rendering resolutions from 360p to 1440p and reduced CPU/rendering overhead.
- [v0.4.2 — Settings and language selection](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.4.2)
  Reorganized launcher settings, added game language selection, and fixed startup failures caused by unsupported system regions.
- [v0.4.3 — Pipeline preparation and handheld improvements](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.4.3)
  Added startup preparation and caching of known graphics pipelines to reduce compilation stalls during gameplay. Improved Windows timing, worker synchronization and CPU overhead, and introduced an optimized Android 10+ APK alongside the Android 9-compatible build.
- [v0.4.4 — Android race timing and CPU improvements](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.4.4)
  Improved Android CPU scheduling and guest execution overhead, corrected race simulation timing at lower frame rates, and fixed Vulkan shader compatibility on GPUs without 64-bit integer support. Racing was verified at approximately 23–24 FPS with time matching real seconds on AYN Thor; Intro movies remain slower than expected.
- [v0.4.5 — Android Intro playback improvements](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.4.5)
  Reduced ARM64 movie-decoding overhead with faster vector memory operations. On the tested AYN Thor, maximum sampled Intro video delay fell from several seconds to under one second, while race timing remained correct. Some playback delay may remain on other devices.
- [v0.4.6 — Course stability and handheld improvements](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.4.6)
  Fixed low-frame-rate traversal failures in Frozen Forest, Final Factory, Metropolis Speedway and Forgotten Tomb, plus loading and long-session stability issues. Improved controller steering and side reach, Android rotation recovery, race UI timing and texture handling. Added Android diagnostic ZIP export and further CPU/rendering optimizations; performance still varies by device.
- [v0.4.7 — Windows CPU optimizations and rendering stability](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.4.7)
  Reduced Windows guest checkpoint overhead and added D3D12 draw recording on a render thread. Fixed stale resolved textures when guest memory is reused and improved cleanup after rendering errors. Added isolated benchmark tooling and expanded rendering regression coverage. Vulkan retains synchronous recording by default; Android performance gains are not established by this release.
- [v0.5.0 — Unleashed/Marathon-style architecture](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.5.0)
  Moved the runtime to the Unleashed/Marathon Recompiled model: one render target per guest surface with deferred and depth resolves (race shadows now appear), direct guest memory access, a host o1heap for the title's heap and physical memory, every guest thread running in parallel with per-subsystem locks instead of a global lock, the render thread on every backend, and real-time race stepping on every platform. On the tested AYN Thor a race went from about 25 to about 49–50 FPS.
- [v0.5.1 — Controller air tricks](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.5.1)
  Fixed air tricks with a controller: turning the left stick in the air now raises a jump's rating, as turning the body does with Kinect. Also integrates a faster guest-memory path for the checked (debugging) build.
- [v0.5.2 — Air trick landing fix](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.5.2)
  Spinning the left stick for an air trick no longer leans or accelerates the board, which had carried trick jumps off the side of Rocky Ridge out of the course.
- [v0.5.3 — Jump physics and Retry fix](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.5.3)
  Desktop races step one original frame at a time again, so charged jumps off the side of Rocky Ridge land back on the course. Retrying a race from the pause menu no longer brings the confirmation back and leaves it stuck on screen.
- [v0.5.4 — Grey flash fix](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.5.4)
  Removed the single frames of flat grey that flashed now and then during races: a HUD sprite the game parks far off screen was sometimes drawn across the whole screen. Added diagnostics for stalls and single-colour frames.
- [v0.5.5 — Mali GPU start-up fix](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.5.5)
  Android phones and tablets with a Mali-G57 GPU (Helio G99, such as the Galaxy Tab A9) no longer crash at start-up: the renderer asked for one more descriptor set than these GPUs allow. The log now names each graphics start-up step.
- [v0.5.6 — Air tricks on side jumps, touch pause menu](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.5.6)
  Spinning the stick in the air raises a side jump's rating again. On touch screens the on-screen stick turns the pause menu's wheel.
- [v0.6.0 — Mod support](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.6.0)
  The game loads file-replacement mods in HedgeModManager's format, the one Unleashed and Marathon Recompiled use. The launcher's new Mods tab switches them on and off and sets their order.
- [v0.6.1 — Two players and Relay Race on controllers](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.6.1)
  Each player turns, confirms and backs out of the two-player menus with their own controller, and either can pause; a guest second player no longer adds a 10-second wait. Relay Race works with controllers: A swaps in the next racer. The launcher's settings can all be reached with a controller. 4-core CPUs keep the main thread's core to itself, and frames over 250 ms are logged.
- [v0.6.2 — Fewer freezes, START skips story scenes](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.6.2)
  The race and Relay hand-over freezes of up to a few seconds are gone: the main thread no longer spins on a lock other threads hold across system calls. START skips a World Grand Prix story scene, and the results screen's Kinect Guide no longer closes the game.
- [v0.6.3 — Any install folder, voices apart from text, steadier sound](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.6.3)
  The game starts from a folder with non-Latin letters on Windows, and full screen works with Vulkan on Linux. Voices can be English or Japanese apart from the text. Sound keeps a short cushion against crackles, and 4-core CPUs no longer pin the main thread. The programs have an icon, and HedgeModManager can find the game and manage its mods.
- [v0.6.4 — No more getting stuck at start with Multi-core execution off](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.6.4)
  With the launcher's Multi-core execution turned off the game got stuck at start on every device. The switch is gone: the game's threads always run in parallel, and an old setting that turned it off is ignored.
- [v0.6.5 — Index buffers kept on the GPU (test build)](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.6.5)
  Index buffers the game leaves unchanged stay on the GPU instead of being decoded and copied for every draw, and cached buffers are found without checking memory views each time. A test build for slower CPUs; `SFR_INDEX_CACHE=0` turns the new cache off.
- [v0.6.6 — Crash records in the Android diagnostic ZIP (test build)](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.6.6)
  On Android 11 and later the diagnostic ZIP now says how the game last ended, with the system's crash record (tombstone) when it crashed in native code, so a crash that leaves nothing in the log can be located. Also has v0.6.5's index cache.

## System Requirements

- **Windows**: Windows 10 or 11 (x64), a CPU with AVX, a GPU with Direct3D 12
  (or Vulkan 1.2).
- **Linux**: x86-64 with AVX and a Vulkan 1.2 driver (tested on Ubuntu 22.04).
- **Android**: Android 9 or later, arm64-v8a, Vulkan 1.1; about 2 GB free
  for the installed game. Releases also offer an **Android 10+** APK using
  native thread-local storage to reduce CPU overhead; prefer that build on
  Android 10 or later. Android performance remains experimental; the latest
  race timing and performance improvements were tested on AYN Thor and have
  not yet been re-tested on Pocket S2 Pro.
- Building needs the tools listed in [docs/building.md](docs/building.md).

## How to Install

1. Download the release for your system from
   [Releases](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases):
   the Windows zip, the Linux tarball, or the Android APK.
   For Android 10+, choose `android10-arm64.apk`; the `android-arm64.apk`
   package retains Android 9 compatibility. Both use the same app identity
   and update existing installations without requiring an uninstall.
2. Unpack it into a folder of its own (on Android, install the APK).
3. Start `FreeRidersRecompiled` and choose your disc image (`.iso`) when the
   launcher asks; it copies the game's data beside itself. Then press
   **Start game**.

## Settings

The launcher groups settings by what you want to change:

| Category | Settings |
| --- | --- |
| General | Launcher, game and voice languages, movies, restore defaults |
| Graphics | Window and rendering resolutions, fullscreen, backend, VSync, expandable performance options |
| Sound | Game volume and launcher sounds |
| Controls | Each player's input source and controller, button/key bindings, Android touch and tilt |
| Motion input | Kinect/webcam mode, device and preview, mirror, skeleton debug, voice commands |
| Avatar models | Select or clear a custom VRM/GLB model |
| Mods | Switch file-replacement mods on and off and set their order ([Mods](docs/mods.md)) |
| Game files | Installation, data locations and shader pack |

Window and rendering resolutions sit together under **Graphics > Resolution**:
the first sets the output window size, while the second controls the pixels the
game draws. Changes to game settings apply on the next game launch.

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

## Kinect and Kinect v2

The Windows version supports physical **Kinect v1** and **Kinect v2** sensors
for body tracking. Kinect is optional: controller and keyboard play do not
require a sensor or its SDK.

| Sensor | What you need | Current status |
| --- | --- | --- |
| Kinect for Xbox 360 / Kinect for Windows v1 | SDK 1.8 and a powered USB connection; the Xbox 360 sensor requires the full SDK, while Kinect for Windows v1 can use Runtime 1.8 | Xbox 360 Kinect tested through races on Windows 10 |
| Kinect v2 / Kinect for Xbox One | SDK 2.0, USB 3.0 and a compatible powered PC adapter | Experimental; skeleton tracking is implemented, but hardware gameplay validation is still pending |

Install the matching SDK, connect the sensor, then select **Kinect** in the
launcher's **Motion input > Camera** setting and choose **Open preview** to check tracking.
Both generations use the same option: the runtime tries v1 first, then v2.
Keep your whole body visible
and allow enough space to move.

Kinect v1 can supply skeleton, depth and color data. The current v2 backend
supplies skeletons only, with additional skeleton-based steering and crouch/jump
handling. Physical Kinect input is Windows-only; Linux and Android releases
do not provide it.

SDK links, adapter requirements and troubleshooting:
[Kinect sensor guide](docs/kinect-sensor.md) (Traditional Chinese).
For motion controls with an ordinary webcam, see the next section.

## Camera motion input

![Camera-controlled gameplay alongside the live skeleton debug window](docs/images/camera-input-skeleton.png)

*Gameplay and the skeleton delivered to the game, cropped from the same
recording frame and arranged side by side. The debug window shows front and
side views of the joints, without displaying the webcam image.*

Windows releases include the models and runtime for webcam motion
input. Select **Motion input > Camera > Motion** in the launcher and keep your whole body
in view. **Skeleton debug window** opens front and side views of the joints
the game receives; it does not show the camera image.

Camera input controls 1P. Using the controller takes priority; after 1.5
seconds without controller input, fresh camera tracking resumes. If tracking
is lost, controller input remains available. Set 2P input to **Off** for
single-player play.

Inference runs locally using [ONNX Runtime](https://github.com/microsoft/onnxruntime)
1.30.0 (MIT), with the [OpenCV Zoo MediaPipe pose model](https://github.com/opencv/opencv_zoo/tree/47534e27c9851bb1128ccc0102f1145e27f23f98/models/pose_estimation_mediapipe)
and [person detector](https://github.com/opencv/opencv_zoo/tree/47534e27c9851bb1128ccc0102f1145e27f23f98/models/person_detection_mediapipe)
(Apache 2.0). These models originate from Google's MediaPipe; this port uses
their ONNX exports, without requiring the MediaPipe or OpenCV runtime.
Model downloads are pinned by SHA-256 in
[scripts/fetch_pose_model.py](scripts/fetch_pose_model.py). Licences and
third-party notices are included in the Windows archive's `licenses/` folder.

A webcam estimates relative 3D pose; it does not measure depth like Kinect.
The Linux and Android prebuilt packages do not enable camera motion input.
Setup, gestures, source builds and limitations:
[Camera input](docs/camera-input.md).

## VRM Avatar models

Starting with v0.3.0, you can use your own **VRM 0.x / VRM 1.0** model for the
in-game **AVATAR** rider. Camera motion input is optional; controllers and
keyboard controls work with the model too.

![A custom VRM model riding an Extreme Gear during a race](docs/images/custom-vrm-gameplay.png)

*A custom VRM model replacing the in-game AVATAR rider.*

1. Open the launcher's **Avatar models** tab.
2. Choose **Browse** and select a `.vrm` or binary `.glb` file. Desktop users
   can also type its path; Android imports a copy into the app's storage.
3. Start the game and select **AVATAR** in the character menu, then choose
   your Gear. AVATAR must already be available in your game save; selecting
   a model does not unlock it.
4. To change models, return to the launcher and restart the game. **Clear**
   disables the custom model.

One model setting is shared by both local players; it applies to whoever
selects AVATAR, with separate poses for each player. Ordinary characters are
unchanged. The model appears in Loading and races, follows the game's main
body animation, jumps and stance changes, and supplies hand positions for
held items. Models are read locally; no avatar model is bundled or uploaded.

This is experimental. A `.glb` without VRM humanoid bones stays static.
Full MToon shading, transparent blending, spring bones, facial expressions
and finger animation are not implemented. Some poses and item grips can
look different between models, and complex models can reduce performance.
Windows D3D12/Vulkan gameplay has been checked; full two-player VRM gameplay
and physical Linux/Android VRM gameplay still need broader testing.

Troubleshooting and platform details: [VRM Avatar guide](docs/vrm-avatar.md).

## FAQ

**How do I select Spanish or another game language?** Open **General > Game language**
in the launcher and choose English, Japanese, German, French, Spanish or Italian,
then start the game. **System language** is the default. This is separate from
the launcher's English/Traditional Chinese interface. Unsupported system languages
use English; unavailable or unmapped system countries use the US profile and
record a warning in `game.log`, without requiring changes to your OS settings.
For direct runtime launches, set `SFR_GAME_LANGUAGE` to `auto`, `en`, `ja`, `de`,
`fr`, `es` or `it`. Invalid values behave as `auto`.

**How do I choose Vulkan or D3D12?** On Windows, open **Graphics > Graphics backend**
and choose either backend before starting the game. New settings default to Vulkan;
existing saved choices are preserved. Linux and Android use Vulkan.

**Can I change the game's internal resolution?** Yes. **Rendering resolution**
offers 360p, 540p, native 720p (default), 1080p and 1440p, independently of output
window size. Apply it before starting the game. Lower values may help a limited
GPU; higher values draw more detail at a greater cost. See
[Internal resolution](docs/internal-resolution.md).

**How can I compare performance without changing game speed?** Keep the normal
60 FPS cap and compare the same part of the same course, including slower-frame
times. Uncapping currently speeds up the game logic too. Recording instructions
and the limits of testing on a faster PC: [Performance measurements](docs/benchmarking.md).
Guided 1P, 2P, Camera and VRM comparisons: [Benchmark scenarios](docs/benchmark-scenarios.md).
Current local verification and hardware limits: [Validation report](docs/performance-resolution-validation.md).

**Why is there a preparation screen before the game starts?** Builds with a pipeline
manifest prepare known graphics pipelines before the first game frame, reducing
first-use compilation stalls. The first start may take longer; later starts reuse
this device's cache. Keep the `pipeline-cache` folder. Close the window or press
Escape to cancel (hold Back on Android). Unrecorded combinations can still compile
during play. See [Pipeline preparation](docs/pipeline-preparation.md).

**Where are the settings and saves?** Beside the launcher: `settings.ini`,
`save/` and `game.log` (on Android, in the app's files directory). The game
plays as a local profile named "Player"; answer *Yes* when it asks "Are you
Player?" and create save data when offered.

**Why does the game need `shaders.pack` on Linux and Android?** The Xbox
shaders are translated with Windows tools while the game runs. The pack
carries them, already translated, to machines without those tools; the game
creates every shader it has while it boots, so a pack made with
`python scripts/pack_shaders.py` after starting the game once on Windows
(under Vulkan too, for the SPIR-V) is complete. Releases include one.

**Can I use mods?** File-replacement mods, yes, since v0.6.0: put each in its
own folder in `mods` beside the launcher and switch it on in the **Mods** tab
([Mods](docs/mods.md)). Sound, texture and text mods made for the original
game work once their files sit in a folder with a `mod.ini`; for example the
[Traditional Chinese mod](https://github.com/YuutaTsubasa/Free-Riders-Recompiled-Traditional-Chinese-Mod).
Code mods do not apply: the No Kinect Patch changes the original executable
for Xenia, and the Kinect emulation here is the project's own code (see
[Credits](#credits)).

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
  kernel's behaviour and GPU texture formats/layouts.
- [ONNX Runtime](https://github.com/microsoft/onnxruntime) (camera pose inference),
  [OpenCV Zoo](https://github.com/opencv/opencv_zoo) and
  [MediaPipe](https://github.com/google-ai-edge/mediapipe) (person detection and
  3D pose models). Dependency versions and licences: [THIRD_PARTY.md](THIRD_PARTY.md).
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
