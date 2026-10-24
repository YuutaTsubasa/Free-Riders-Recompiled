#!/usr/bin/env python3
"""Pack the runtime shader cache into one file the game reads without tools.

Every entry of out/shaders/runtime (the current shader ABI key) that has been
compiled is written with its original container, specialization mask and
whichever bytecode exists: DXIL (built by a D3D12 run) and SPIR-V (built by a
Vulkan run, SFR_GRAPHICS=vulkan). The game consults the pack (SFR_SHADER_PACK,
default out/shaders/shaders.pack) before translating anything, so a machine
with the pack needs neither the translator nor a DXC for those shaders.

The pack holds shaders taken from the local game files: like image.bin it is
private output, never committed or distributed.

Format, little-endian: b"SFRSHPK2", u32 shader ABI, u32 count, then per entry u32 stage
(0 vertex, 1 pixel), u32 specialization mask, u32 source, legacy DXIL, SPIR-V
and raw DEC3N DXIL sizes, followed by those bytes. Vertex DXIL is a complete
legacy/raw pair; a Vulkan-only entry has neither DXIL payload.
"""
import argparse
import struct
import subprocess
import sys
from pathlib import Path
from shader_pack_format import SHADER_ABI, header, validate_shader_pack
from prepare_shaders import verify_translator

ROOT = Path(__file__).resolve().parents[1]
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


def compile_dec3n(folder, translator):
    raw_hlsl = folder / 'shader.dec3n.hlsl'
    result = subprocess.run([str(translator), '--dec3n-variant', str(folder / 'shader.hlsl'), str(raw_hlsl)],
                            capture_output=True, text=True)
    (folder / 'dec3n_variant.log').write_text(result.stdout + result.stderr)
    if result.returncode:
        raise ValueError(f'{folder}: DEC3N vertex adapter failed: {result.stderr.strip()}')
    result = subprocess.run([str(DXC), '-T', 'lib_6_3', '-HV', '2021', '-all-resources-bound',
                             '-Wno-ignored-attributes', '-Qstrip_reflect', '-Qstrip_debug',
                             '-Fo', str(folder / 'shader.dec3n.dxil'), str(raw_hlsl)], capture_output=True, text=True)
    (folder / 'compile_dec3n.log').write_text(result.stdout + result.stderr)
    if result.returncode:
        raise ValueError(f'{folder}: DEC3N vertex library compilation failed: {result.stderr.strip()}')
    return read(folder / 'shader.dec3n.dxil')


def read(path):
    return path.read_bytes() if path.exists() else b''


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--cache', type=Path, default=ROOT / 'out/shaders/runtime')
    parser.add_argument('--output', type=Path, default=ROOT / 'out/shaders/shaders.pack')
    parser.add_argument('--compile-dxil', action='store_true',
                        help='compile the DXIL a Vulkan-only run left out (Windows: the dxc.exe of dxc-bin)')
    parser.add_argument('--translator', type=Path, default=ROOT / 'out/tools/shader-translator/shader_translate.exe')
    parser.add_argument('--vulkan-only', action='store_true',
                        help='pack only SPIR-V; Vulkan keeps the legacy CPU DEC3N path')
    args = parser.parse_args()
    if args.vulkan_only and args.compile_dxil:
        parser.error('--vulkan-only and --compile-dxil are mutually exclusive')
    if args.compile_dxil:
        verify_translator(args.translator)
    if args.compile_dxil and not DXC.exists():
        sys.exit(f'--compile-dxil needs {DXC}')
    entries = []
    for folder in sorted(args.cache.glob(f'v{SHADER_ABI}-*')):
        source = read(folder / 'original.bin')
        mask_text = read(folder / 'specialization_mask.txt').strip()
        if not source or not mask_text or (folder / 'untranslatable.txt').exists():
            continue
        dxil, spirv, dec3n = (read(folder / name) for name in ('shader.dxil', 'shader.spv', 'shader.dec3n.dxil'))
        if not dxil and not spirv:
            continue
        if len(source) < 4:
            raise ValueError(f'{folder}: truncated original shader container')
        stage = 0 if struct.unpack_from('>I', source, 0)[0] & 1 else 1
        if args.vulkan_only:
            if not spirv:
                continue
            dxil = dec3n = b''
        elif args.compile_dxil:
            if not dxil:
                dxil = compile_dxil(folder, stage, int(mask_text))
            if not dxil:
                raise ValueError(f'{folder}: legacy DXIL compilation failed')
            if stage == 0 and not dec3n:
                dec3n = compile_dec3n(folder, args.translator)
        if stage == 0 and bool(dxil) != bool(dec3n):
            raise ValueError(f'{folder}: vertex DXIL requires its paired DEC3N library; use --compile-dxil or --vulkan-only')
        if stage == 1 and dec3n:
            raise ValueError(f'{folder}: pixel DEC3N payload is forbidden')
        entries.append((stage, int(mask_text), source, dxil, spirv, dec3n))
    out = bytearray(header(len(entries)))
    for stage, mask, source, dxil, spirv, dec3n in entries:
        out += struct.pack('<6I', stage, mask, len(source), len(dxil), len(spirv), len(dec3n))
        out += source + dxil + spirv + dec3n
    validate_shader_pack(out)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix('.pack.new')
    temporary.write_bytes(out)
    temporary.replace(args.output)
    print(f'{args.output}: {len(entries)} shaders '
          f'({sum(1 for e in entries if e[3])} DXIL, {sum(1 for e in entries if e[4])} SPIR-V), {len(out)} bytes')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        sys.exit(str(error))
