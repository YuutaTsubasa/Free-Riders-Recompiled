"""Prepare known basic and optional embedded renderer shaders in a private cache.

The asset digest is a compatibility gate, not a substitute for bounds checking.
Only the pinned translator is supported; this is not a general Xenos validator.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[1]
ASSET_SHA256 = 'b4a865e23ecb7eed2770233d9b865923180d084a483b6d2f0c44cf54d73848b2'
IMAGE_BASE = 0x82000000
IMAGE_SIZE = 34209792
IMAGE_SHA256 = 'd092c5397d769594d7b46cda023cf2c62aed9df4e76db44efee8136daf00ad99'
EMBEDDED_POINTER_TABLE = 0x82AF6D30
EMBEDDED_SHADERS = (
    dict(guest_address=0x820D2600, size=296, stage='vertex', source_sha256='f563f13062eb4ae4a414f6509b5dcf4abb3837fed62b65eb220898734ac388d1'),
    dict(guest_address=0x820D2728, size=284, stage='vertex', source_sha256='8942ad3cc544af5b4ff186f762010e24d69340ec30a4d9fe0780304d3d028474'),
    dict(guest_address=0x820D2848, size=320, stage='vertex', source_sha256='86c79b8000206e479be9a5b964f0c6c9c6e03889297ee7894e78ad3a868cfac1'),
    dict(guest_address=0x820D2988, size=160, stage='pixel', source_sha256='f098278a3b50fbeec562599620580322b5fa223c389ca2c222215a13399f5247'),
    dict(guest_address=0x820D2A28, size=232, stage='pixel', source_sha256='2302efea6ffd5c9e20ebebe71af9ad9617cb9483fb1d919b9296248e5fc8dd28'),
    dict(guest_address=0x820D2B10, size=232, stage='pixel', source_sha256='d6c0aeb178913e74870fc3dfb7ba209536c6a62400d2204a198d015198b76ea0'),
)
TOOL_PIN = 'fb32631ee398e46f2a113d8f9103201dbaa000b4'
TOOL_ROOT = ROOT / 'tools/XenosRecomp'
COMMON = TOOL_ROOT / 'XenosRecomp/shader_common.h'
UPSTREAM_NAMES = ('constant_table.h', 'shader.h', 'shader_code.h', 'shader_common.h', 'shader_recompiler.h', 'shader_recompiler.cpp', 'pch.h')
TRANSLATOR_SOURCES = tuple('tools/XenosRecomp/XenosRecomp/' + name for name in UPSTREAM_NAMES) + ('src/shader_translate_main.cpp',)
# XenosRecomp's pinned dxc-bin, as everywhere else: the game links these
# libraries at run time with the same DXC (a release ships it), and DXC refuses
# to link libraries from another version.
DXC = ROOT / 'tools/XenosRecomp/thirdparty/dxc-bin/bin/x64/dxc.exe'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def check(ok, message):
    if not ok:
        raise ValueError(message)


def scan_containers(data):
    records = []
    def u32(buf, p):
        return struct.unpack_from('>I', buf, p)[0]
    for base in range(0, len(data) - 35, 4):
        if u32(data, base) & 0xffffff00 != 0x102a1100:
            continue
        flags, vs, ps, _, ct, dt, sh, reserved1, reserved2 = struct.unpack_from('>9I', data, base)
        check(vs >= 36 and ps > 0 and base + vs + ps <= len(data), 'container extent')
        check(reserved1 == reserved2 == 0, 'container reserved fields')
        check(not records or records[-1]['offset'] + records[-1]['size'] <= base, 'overlapping containers')
        buf = data[base:base + vs + ps]
        def vr(p, n):
            check(0 <= p <= vs and 0 <= n <= vs - p, 'virtual range outside container')
        def pr(p, n):
            check(0 <= p <= ps and 0 <= n <= ps - p, 'physical range outside container')
        def zstr(p):
            vr(p, 1)
            end = buf.find(b'\0', p, vs)
            check(end >= p, 'unterminated string')
            return buf[p:end].decode('ascii')
        vr(sh, 24)
        po, size, _, _, _, interp = struct.unpack_from('>6I', buf, sh)
        pr(po, size)
        check(size > 0 and size % 12 == 0, 'instruction block multiple of 12')
        stage = 'vertex' if flags & 1 else 'pixel'
        ni = (interp >> 5) & 31
        if stage == 'vertex':
            vr(sh, 36)
            skip, ne, _ = struct.unpack_from('>3I', buf, sh + 24)
            vr(sh + 36, 4 * (skip + ne + ni))
            for j in range(ne):
                check(((u32(buf, sh + 36 + 4 * (skip + j)) >> 12) & 15) <= 13, 'vertex usage')
        else:
            vr(sh, 32 + 4 * ni)
            check(u32(buf, sh + 28) & ~31 == 0, 'pixel outputs mask')
        vr(ct, 32)
        csz, tsz, creator, _, nc, ci, _, target = struct.unpack_from('>8I', buf, ct)
        vr(ct, csz)
        check(csz >= 32 and tsz == 28, 'constant table size')
        cb = ct + 4
        vr(cb + ci, nc * 20)
        zstr(cb + creator)
        if target:
            zstr(cb + target)
        for j in range(nc):
            name, rs, _, _, reserved, ti, dv = struct.unpack_from('>I4H2I', buf, cb + ci + j * 20)
            check(rs <= 3 and reserved == 0, 'constant register metadata')
            zstr(cb + name)
            vr(cb + ti, 16)
            pc, pt, _, _, _, members, _ = struct.unpack_from('>6HI', buf, cb + ti)
            check(pc <= 5 and pt <= 19 and members == 0, 'unsupported constant type')
            if dv:
                vr(cb + dv, 1)
        if dt:
            vr(dt, 20)
            dsz = u32(buf, dt + 16)
            vr(dt, dsz)
            check(dsz >= 20, 'default table size')
            dp = dt + 20
            while True:
                vr(dp, 4)
                if u32(buf, dp) == 0:
                    break
                vr(dp, 8)
                _, count, dpo = struct.unpack_from('>HHI', buf, dp)
                pr(dpo, ((count + 3) // 4) * 16)
                dp += 8
            dp += 4
            while True:
                vr(dp, 4)
                if u32(buf, dp) == 0:
                    break
                _, count = struct.unpack_from('>HH', buf, dp)
                vr(dp, 8 + count * 4)
                dp += 8 + count * 4
        code = buf[vs + po:vs + po + size]
        end, pos, ops, clauses = size, 0, [], []
        while pos < end:
            check(pos + 12 <= size, 'control-flow extent')
            w0, w1, w2 = struct.unpack_from('>3I', code, pos)
            for value in (w0 | ((w1 & 65535) << 32), (w1 >> 16) | (w2 << 16)):
                op = (value >> 44) & 15
                ops.append(op)
                if op in (1, 2, 3, 4, 5, 6, 13, 14):
                    addr, count = value & 4095, (value >> 12) & 7
                    check(count > 0 and (addr + count) * 12 <= size, 'exec clause extent')
                    clauses.append(addr * 12)
                    if addr:
                        end = min(end, addr * 12)
            pos += 12
        check(pos == end and clauses, 'control-flow boundary')
        check(all(a >= end for a in clauses), 'exec overlaps control flow')
        check(any(op in (2, 4, 6, 14) for op in ops), 'missing end opcode')
        records.append(dict(offset=base, size=vs + ps, stage=stage, source_sha256=sha(buf)))
    return records


def validate_asset(data):
    check(sha(data) == ASSET_SHA256, 'Unsupported asset SHA256; expected the known Xbox360BasicShader.fxobj')
    records = scan_containers(data)
    check(len(records) == 68, 'Expected 68 bounded shader containers')
    return records


def validate_embedded_image(data):
    check(len(data) == IMAGE_SIZE, 'Unsupported decoded image size')
    check(sha(data) == IMAGE_SHA256, 'Unsupported decoded image SHA256')
    check(len(EMBEDDED_SHADERS) == 6, 'Expected exactly six embedded shader records')
    table = EMBEDDED_POINTER_TABLE - IMAGE_BASE
    check(0 <= table <= len(data) - 24, 'embedded pointer table extent')
    pointers = struct.unpack_from('>6I', data, table)
    records, ranges = [], []
    for pointer, expected in zip(pointers, EMBEDDED_SHADERS):
        address, size, stage = expected['guest_address'], expected['size'], expected['stage']
        offset = address - IMAGE_BASE
        check(stage in ('vertex', 'pixel'), 'embedded shader stage')
        check(address % 4 == 0 and 0 <= offset <= len(data) and 36 <= size <= len(data) - offset,
              'embedded container extent')
        check(pointer == address, 'embedded shader pointer table mismatch')
        check(all(offset + size <= lo or offset >= hi for lo, hi in ranges),
              'duplicate or overlapping embedded containers')
        ranges.append((offset, offset + size))
        source = data[offset:offset + size]
        check(sha(source) == expected['source_sha256'], 'embedded source SHA256 mismatch')
        check(struct.unpack_from('>I', source)[0] == (0x102A1101 if stage == 'vertex' else 0x102A1100),
              'embedded shader stage mismatch')
        parsed = scan_containers(source)
        check(len(parsed) == 1 and parsed[0]['offset'] == 0 and parsed[0]['size'] == size
              and parsed[0]['stage'] == stage, 'embedded container size/stage mismatch')
        records.append(dict(parsed[0], offset=offset, guest_address=address, source_kind='decoded_image'))
    return records


def compiler_profile(stage, mask):
    check(stage in ('vertex', 'pixel') and 0 <= mask <= 0xffffffff, 'invalid shader metadata')
    if mask:
        return ['-T', 'lib_6_3']
    return ['-T', 'vs_6_0' if stage == 'vertex' else 'ps_6_0', '-E', 'shaderMain']


def validate_dxil(data, stage, mask):
    check(len(data) >= 32 and data[:4] == b'DXBC', 'invalid DXBC header')
    version, size, count = struct.unpack_from('<III', data, 20)
    check(version == 1 and size == len(data) and 0 < count <= (size - 32) // 4, 'invalid DXBC extent')
    ranges, programs = [], []
    for i in range(count):
        off = struct.unpack_from('<I', data, 32 + 4 * i)[0]
        check(32 + 4 * count <= off <= size - 8, 'invalid DXBC chunk offset')
        length = struct.unpack_from('<I', data, off + 4)[0]
        check(length <= size - off - 8, 'invalid DXBC chunk extent')
        ranges.append((off, off + 8 + length))
        if data[off:off + 4] == b'DXIL':
            programs.append(data[off + 8:off + 8 + length])
    ranges.sort()
    check(all(a[1] <= b[0] for a, b in zip(ranges, ranges[1:])), 'overlapping DXBC chunks')
    check(len(programs) == 1 and len(programs[0]) >= 24, 'missing DXIL program')
    program = programs[0]
    _, kind, words, magic, _, offset, bitcode_size = struct.unpack_from('<HHI4sIII', program)
    expected = 6 if mask else (1 if stage == 'vertex' else 0)
    check(kind == expected, 'compiled shader stage mismatch')
    check(words * 4 == len(program) and magic == b'DXIL', 'invalid DXIL program extent')
    start = 8 + offset
    check(start >= 24 and bitcode_size >= 4 and start + bitcode_size <= len(program), 'invalid DXIL bitcode extent')
    check(program[start:start + 4] == b'BC\xc0\xde', 'invalid DXIL bitcode')


def verify_translator(translator):
    provenance = json.loads(translator.with_suffix('.json').read_text(encoding='utf-8-sig'))
    check(provenance['commit'] == TOOL_PIN, 'translator pin mismatch')
    check(provenance['sha256'] == sha(translator.read_bytes()), 'translator executable hash mismatch')
    check(set(provenance['sources']) == set(TRANSLATOR_SOURCES), 'translator source inventory mismatch')
    actual = subprocess.check_output(['git', '-C', str(TOOL_ROOT), 'rev-parse', 'HEAD'], text=True).strip()
    check(actual == TOOL_PIN, 'XenosRecomp checkout pin mismatch')
    subprocess.check_call(['git', '-C', str(TOOL_ROOT), 'diff', '--quiet', 'HEAD', '--', *('XenosRecomp/' + name for name in UPSTREAM_NAMES)])
    for name, expected in provenance['sources'].items():
        check(sha((ROOT / name).read_bytes()) == expected, 'translator source hash mismatch: ' + name)
    return provenance


def run(args, log):
    args = [str(a) for a in args]
    result = subprocess.run(args, capture_output=True, timeout=60)
    log.write_bytes(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f'{args[0]} exit {result.returncode}: {result.stderr.decode(errors="replace").strip()}')
    return result.stdout


def generate_cpp(records, folder):
    lines = ['// Generated from validated original containers. Keep this file private.', f'// Authoritative provenance: {folder.name}/manifest.json', '#include "shader_cache.h"', 'namespace sfr { namespace {']
    for i, record in enumerate(records):
        for field, filename in [('source', 'original.bin'), ('dxil', 'shader.dxil')]:
            blob = (folder / record['directory'] / filename).read_bytes()
            lines.append(f'static constexpr uint8_t {field}_{i}[] = {{')
            lines.extend(','.join(f'0x{x:02x}' for x in blob[j:j + 24]) + ',' for j in range(0, len(blob), 24))
            lines.append('};')
    lines.append('static const ShaderCacheEntry entries[] = {')
    for i, record in enumerate(records):
        lines.append(f'{{source_{i}, dxil_{i}, ShaderStage::{record["stage"]}, {record["specialization_mask"]}u}},')
    lines += ['};', '} std::span<const ShaderCacheEntry> compiled_shader_cache() { return entries; }', '}']
    return '\n'.join(lines) + '\n'


def publish_cache(cpp, manifest_bytes, output):
    manifest = output / 'manifest.json'
    old_manifest = manifest.read_bytes() if manifest.exists() else None
    temporary = output / ('manifest-' + uuid.uuid4().hex + '.json')
    temporary.write_bytes(manifest_bytes)
    os.replace(temporary, manifest)
    try:
        os.replace(cpp, output / 'shader_cache_data.cpp')
    except OSError:
        if old_manifest is None:
            manifest.unlink()
        else:
            temporary.write_bytes(old_manifest)
            os.replace(temporary, manifest)
        raise


def prepare(asset, translator, dxc, output, image=None):
    data = asset.read_bytes()
    records = [dict(record, source_kind='basic_asset') for record in validate_asset(data)]
    inputs = {'basic_asset': data}
    sources = [dict(kind='basic_asset', size=len(data), sha256=sha(data))]
    if image is not None:
        image_data = image.read_bytes()
        records.extend(validate_embedded_image(image_data))
        inputs['decoded_image'] = image_data
        sources.append(dict(kind='decoded_image', size=len(image_data), sha256=sha(image_data), image_base=IMAGE_BASE))
    identities = [(record['stage'], record['source_sha256']) for record in records]
    check(len(set(identities)) == len(identities), 'duplicate shader identity in combined cache')
    provenance = verify_translator(translator)
    version = subprocess.run([str(dxc), '--version'], capture_output=True, timeout=30, check=True).stdout.decode().strip()
    output.mkdir(parents=True, exist_ok=True)
    # A fresh unique generation cannot accidentally consume files from a prior run.
    generation = output / ('generation-' + uuid.uuid4().hex)
    generation.mkdir()
    results = []
    for record in records:
        name = (f'image-{record["guest_address"]:08x}-{record["stage"]}' if record['source_kind'] == 'decoded_image'
                else f'{record["offset"]:06x}-{record["stage"]}')
        folder = generation / name
        folder.mkdir()
        original, hlsl, dxil = (folder / n for n in ('original.bin', 'shader.hlsl', 'shader.dxil'))
        source_data = inputs[record['source_kind']]
        original.write_bytes(source_data[record['offset']:record['offset'] + record['size']])
        translate_args = [translator, original, hlsl, COMMON]
        stdout = run(translate_args, folder / 'translate.log')
        match = re.fullmatch(rb'(vertex|pixel) HLSL bytes=(\d+) spec_constants_mask=(\d+)\r?\n', stdout)
        check(match is not None and match[1].decode() == record['stage'], 'translator stage mismatch')
        check(hlsl.is_file() and int(match[2]) == hlsl.stat().st_size and hlsl.stat().st_size > 0, 'translator output size mismatch')
        mask = int(match[3])
        args = [dxc, *compiler_profile(record['stage'], mask), '-HV', '2021', '-all-resources-bound', '-Wno-ignored-attributes', '-Qstrip_reflect', '-Qstrip_debug', '-Fo', dxil, hlsl]
        run(args, folder / 'compile.log')
        compiled = dxil.read_bytes()
        validate_dxil(compiled, record['stage'], mask)
        results.append(dict(record, directory=name, specialization_mask=mask, translator_args=[str(x) for x in translate_args], compiler_args=[str(x) for x in args], hlsl_sha256=sha(hlsl.read_bytes()), dxil_sha256=sha(compiled), dxil_bytes=len(compiled)))
    manifest = dict(schema_version=2, sources=sources, asset_sha256=sha(data), tool_pin=TOOL_PIN, translator=provenance, compiler_path=str(dxc), compiler_version=version, compiler_sha256=sha(dxc.read_bytes()), generation=generation.name, records=results)
    cpp = generation / 'shader_cache_data.cpp'
    cpp.write_text(generate_cpp(results, generation), encoding='utf-8')
    manifest['cache_cpp_sha256'] = sha(cpp.read_bytes())
    manifest_path = generation / 'manifest.json'
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    # Publication occurs only after every source and compiler output validates.
    # The immutable generation manifest referenced by the C++ is authoritative,
    # including if the process stops between the two publication replacements.
    publish_cache(cpp, manifest_path.read_bytes(), output)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--asset', type=Path, default=ROOT / 'private/assets/shader/Xbox360BasicShader.fxobj')
    parser.add_argument('--image', type=Path, help='Known decoded game image containing the six renderer shaders')
    parser.add_argument('--translator', type=Path, default=ROOT / 'out/tools/shader-translator/shader_translate.exe')
    parser.add_argument('--dxc', type=Path, default=DXC)
    parser.add_argument('--output-dir', type=Path, default=ROOT / 'out/shaders/basic')
    args = parser.parse_args()
    try:
        manifest = prepare(args.asset.resolve(), args.translator.resolve(), args.dxc.resolve(), args.output_dir.resolve(),
                           image=args.image.resolve() if args.image is not None else None)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        parser.exit(1, f'Shader preparation failed: {exc}\n')
    print(f'Prepared {len(manifest["records"])} shaders: {args.output_dir / "shader_cache_data.cpp"}')


if __name__ == '__main__':
    main()
