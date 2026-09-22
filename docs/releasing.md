# Releasing

Releases follow Unleashed Recompiled's model: the maintainer builds the game
from their own disc and publishes the programs; players install the game's
data from **their own** disc image in the launcher. A release never contains
the disc's files (`default.xex`, the asset archives, `image.bin`), saves, or
anything from `private/` or `game/`. GitHub Actions cannot make a release: it
has no disc, and must never be given one.

What a release does contain, beside the launcher: the recompiled game
(`sfr_cpu_diagnostic`), the `shaders.pack` translated so far, the licences
and, on Windows, the shader tools that translate shaders the pack lacks.

## 1. Build

On Windows, from a clean tree at the commit being released:

```powershell
./scripts/build_shader_translator.ps1
./scripts/build_tools.ps1 -Diagnostic
python scripts/pack_shaders.py            # out/shaders/shaders.pack
```

Play through what the release should cover first (each run adds the shaders
it meets to `out/shaders/runtime`), and run it under Vulkan too
(`SFR_GRAPHICS=vulkan`, or the launcher's Advanced tab), since the pack only
holds SPIR-V for shaders a Vulkan run compiled. Linux and Android have no
shader translator: what the pack lacks, they cannot draw.

Linux (WSL is fine), then Android:

```bash
scripts/build_linux.sh --diagnostic out/recomp/diagnostic
scripts/build_android.sh --diagnostic out/recomp/diagnostic --abi arm64-v8a --pack out/shaders/shaders.pack
```

The APK is signed with the local debug key (`~/.android/debug.keystore`).
Keep that key: Android only installs an update signed with the same one.

## 2. Package

```bash
python scripts/package_release.py windows --version 0.1.0
python scripts/package_release.py linux --version 0.1.0 --build ~/sfr-build
python scripts/package_release.py android --version 0.1.0
```

Each prints the SHA-256 of what it wrote to `out/release/`. The script stops
when a licence file is missing; the DirectX Shader Compiler's texts are kept in
`packaging/licenses/` because dxc-bin ships none.

## 3. Check

Unpack the Windows archive into an empty folder **outside the checkout** (so
the launcher cannot find `tools/` or `out/` above it), install from a disc
image, and play into a race with both D3D12 and Vulkan. `game.log` beside the
launcher must have no `untranslatable=1` line.

## 4. Publish

Tag the commit (`v0.1.0`), push the tag, and create the GitHub release with
the archives and their SHA-256 sums, marked as a pre-release while the game is
incomplete. Say in the notes which disc is supported and that the game data is
not included.
