#!/usr/bin/env python3
"""Pack the runtime shader cache into one file the game reads without tools.

Every entry of out/shaders/runtime (the current "v8-" key) that has been
compiled is written with its original container, specialization mask and
whichever bytecode exists: DXIL (built by a D3D12 run) and SPIR-V (built by a
Vulkan run, SFR_GRAPHICS=vulkan). The game consults the pack (SFR_SHADER_PACK,
default out/shaders/shaders.pack) before translating anything, so a machine
with the pack needs neither the translator nor a DXC for those shaders.

The pack holds shaders taken from the local game files: like image.bin it is
private output, never committed or distributed.

Format, little-endian: b"SFRSHPK1", u32 count, then per entry u32 stage
(0 vertex, 1 pixel), u32 specialization mask, u32 source, DXIL and SPIR-V
sizes, followed by those bytes.
"""
import argparse
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAGIC = b'SFRSHPK1'
DXC = ROOT / 'tools/XenosRecomp/thirdparty/dxc-bin/bin/x64/dxc.exe'


def compile_dxil(folder, stage, mask):
    """DXIL for a shader a Vulkan run translated, exactly as the runtime
    compiles it (runtime_shader_cache.cpp) with the same dxc-bin."""
    profile = ['-T', 'lib_6_3'] if mask else ['-T', 'vs_6_0' if stage == 0 else 'ps_6_0', '-E', 'shaderMain']
    result = subprocess.run([str(DXC), *profile, '-HV', '2021', '-all-resources-bound', '-Wno-ignored-attributes',
                             '-Qstrip_reflect', '-Qstrip_debug', '-Fo', str(folder / 'shader.dxil'),
                             str(folder / 'shader.hlsl')], capture_output=True, text=True)
    (folder / 'compile.log').write_text(result.stdout + result.stderr)
    return read(folder / 'shader.dxil') if result.returncode == 0 else b''


def read(path):
    return path.read_bytes() if path.exists() else b''


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--cache', type=Path, default=ROOT / 'out/shaders/runtime')
    parser.add_argument('--output', type=Path, default=ROOT / 'out/shaders/shaders.pack')
    parser.add_argument('--compile-dxil', action='store_true',
                        help='compile the DXIL a Vulkan-only run left out (Windows: the dxc.exe of dxc-bin)')
    args = parser.parse_args()
    if args.compile_dxil and not DXC.exists():
        sys.exit(f'--compile-dxil needs {DXC}')
    entries = []
    for folder in sorted(args.cache.glob('v8-*')):
        source = read(folder / 'original.bin')
        mask_text = read(folder / 'specialization_mask.txt').strip()
        if not source or not mask_text or (folder / 'untranslatable.txt').exists():
            continue
        dxil, spirv = read(folder / 'shader.dxil'), read(folder / 'shader.spv')
        if not dxil and not spirv:
            continue
        stage = 0 if struct.unpack_from('>I', source, 0)[0] & 1 else 1
        if not dxil and args.compile_dxil and (folder / 'shader.hlsl').exists():
            dxil = compile_dxil(folder, stage, int(mask_text))
        entries.append((stage, int(mask_text), source, dxil, spirv))
    out = bytearray(MAGIC + struct.pack('<I', len(entries)))
    for stage, mask, source, dxil, spirv in entries:
        out += struct.pack('<5I', stage, mask, len(source), len(dxil), len(spirv))
        out += source + dxil + spirv
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix('.pack.new')
    temporary.write_bytes(out)
    temporary.replace(args.output)
    print(f'{args.output}: {len(entries)} shaders '
          f'({sum(1 for e in entries if e[3])} DXIL, {sum(1 for e in entries if e[4])} SPIR-V), {len(out)} bytes')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
