"""Shader pack wire format shared by the producer and release packagers."""
import struct

# Keep in sync with src/shader_pack_format.h and the runtime cache key.
MAGIC = b'SFRSHPK2'
SHADER_ABI = 10


def header(count):
    return MAGIC + struct.pack('<II', SHADER_ABI, count)


def validate_shader_pack(data):
    if len(data) < 16 or data[:8] != MAGIC:
        raise ValueError('shader pack is outdated or invalid; rebuild it with scripts/pack_shaders.py')
    abi, count = struct.unpack_from('<II', data, 8)
    if abi != SHADER_ABI:
        raise ValueError(f'shader pack ABI {abi} does not match {SHADER_ABI}')
    if not count:
        raise ValueError('shader pack contains no shaders')
    at = 16
    for _ in range(count):
        if len(data) - at < 20:
            raise ValueError('shader pack is truncated')
        stage, mask, source, dxil, spirv = struct.unpack_from('<5I', data, at)
        at += 20
        if stage > 1 or not source or not (dxil or spirv):
            raise ValueError('shader pack has an invalid entry')
        size = source + dxil + spirv
        if size > len(data) - at:
            raise ValueError('shader pack is truncated')
        at += size
    if at != len(data):
        raise ValueError('shader pack has trailing data')


def checked_shader_pack(path):
    try:
        validate_shader_pack(path.read_bytes())
    except (OSError, ValueError) as error:
        raise SystemExit(f'{path}: {error}') from error
    return path
