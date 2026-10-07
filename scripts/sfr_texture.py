#!/usr/bin/env python3
"""List, extract and replace the DDS textures inside the game's files.

    python scripts/sfr_texture.py list <file>
    python scripts/sfr_texture.py extract <file> <directory>          (one PNG a texture)
    python scripts/sfr_texture.py replace <file> <n>=<png> [...] -o <output>

The decompressed files (scripts/unpack_assets.py) keep their textures as
plain DDS: DXT1, DXT3, DXT5 or A8R8G8B8, no mipmaps in the ones seen so far.
replace encodes the PNG to the texture's own format and size and writes it
over the old one in place, so nothing else in the file moves. The DXT
encoder is simple (endpoints from each block's range) but good enough for
text and flat artwork. Nothing here contains game data.
"""
import io
import re
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

HEADER = 128


def textures(data):
    """(offset, width, height, format, mipmaps) of every DDS in the data."""
    out = []
    for m in re.finditer(rb'DDS \x7c\x00\x00\x00', data):
        at = m.start()
        height, width = struct.unpack('<II', data[at + 12:at + 20])
        mips = struct.unpack('<I', data[at + 28:at + 32])[0]
        fourcc = data[at + 84:at + 88]
        bits = struct.unpack('<I', data[at + 88:at + 92])[0]
        fmt = fourcc.decode() if fourcc.strip(b'\0') else ('ARGB' if bits == 32 else f'RGB{bits}')
        out.append((at, width, height, fmt, mips))
    return out


def body_size(width, height, fmt):
    blocks = ((width + 3) // 4) * ((height + 3) // 4)
    return {'DXT1': blocks * 8, 'DXT3': blocks * 16, 'DXT5': blocks * 16, 'ARGB': width * height * 4}[fmt]


def to_565(rgb):
    r, g, b = (rgb[..., 0] * 31 + 127) // 255, (rgb[..., 1] * 63 + 127) // 255, (rgb[..., 2] * 31 + 127) // 255
    return (r << 11) | (g << 5) | b


def from_565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return np.stack([(r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31], -1)


def blocks_of(rgba):
    h, w, _ = rgba.shape
    b = rgba.reshape(h // 4, 4, w // 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4)
    return b.astype(np.int64)


def palette_of(c0, c1):
    p0, p1 = from_565(c0), from_565(c1)
    return np.stack([p0, p1, (2 * p0 + p1) // 3, (p0 + 2 * p1) // 3], 1)  # (n, 4, 3)


def encode_colour(blocks, opaque_only):
    """Endpoints on each block's principal axis, then refined by least squares."""
    rgb, alpha = blocks[..., :3].astype(np.float64), blocks[..., 3]
    weight = (alpha > 0 if opaque_only else np.ones(alpha.shape, bool)).astype(np.float64)
    empty = weight.sum(1) == 0
    weight[empty] = 1
    total = weight.sum(1, keepdims=True)
    mean = (rgb * weight[..., None]).sum(1) / total
    centred = (rgb - mean[:, None]) * weight[..., None]
    cov = np.einsum('npi,npj->nij', centred, rgb - mean[:, None])
    axis = np.ones((len(blocks), 3)) / np.sqrt(3)
    for _ in range(8):
        axis = np.einsum('nij,nj->ni', cov, axis)
        axis /= np.maximum(np.linalg.norm(axis, axis=1, keepdims=True), 1e-9)
    proj = np.einsum('npi,ni->np', rgb - mean[:, None], axis)
    big = np.where(weight > 0, proj, -np.inf).max(1)
    small = np.where(weight > 0, proj, np.inf).min(1)
    hi = np.clip(mean + axis * big[:, None], 0, 255)
    lo = np.clip(mean + axis * small[:, None], 0, 255)
    for _ in range(2):
        c0, c1 = to_565(hi.round().astype(np.int64)), to_565(lo.round().astype(np.int64))
        palette = palette_of(c0, c1)
        index = ((rgb[:, :, None, :] - palette[:, None, :, :]) ** 2).sum(-1).argmin(-1)
        # Least squares for the two endpoints given each pixel's weight on them.
        t = np.array([0.0, 1.0, 1 / 3, 2 / 3])[index]  # share of the second endpoint
        a, b = (1 - t) * weight, t * weight
        aa, bb, ab = (a * (1 - t)).sum(1), (b * t).sum(1), (a * t).sum(1)
        ax = (a[..., None] * rgb).sum(1)
        bx = (b[..., None] * rgb).sum(1)
        det = aa * bb - ab * ab
        ok = np.abs(det) > 1e-6
        new_hi = (bb[:, None] * ax - ab[:, None] * bx) / np.where(ok, det, 1)[:, None]
        new_lo = (aa[:, None] * bx - ab[:, None] * ax) / np.where(ok, det, 1)[:, None]
        hi = np.where(ok[:, None], np.clip(new_hi, 0, 255), hi)
        lo = np.where(ok[:, None], np.clip(new_lo, 0, 255), lo)
    c0, c1 = to_565(hi.round().astype(np.int64)), to_565(lo.round().astype(np.int64))
    swap = c0 < c1
    c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
    palette = palette_of(c0, c1)
    index = ((rgb[:, :, None, :] - palette[:, None, :, :]) ** 2).sum(-1).argmin(-1)
    index[c0 == c1] = 0
    bits = (index << (2 * np.arange(16))).sum(1)
    return c0.astype('<u2'), c1.astype('<u2'), bits.astype('<u4')


def encode_dxt(rgba, fmt):
    blocks = blocks_of(rgba)
    c0, c1, bits = encode_colour(blocks, fmt != 'DXT1')
    colour = np.zeros(len(blocks), dtype=[('c0', '<u2'), ('c1', '<u2'), ('bits', '<u4')])
    colour['c0'], colour['c1'], colour['bits'] = c0, c1, bits
    if fmt == 'DXT1':
        return colour.tobytes()
    alpha = blocks[..., 3]
    if fmt == 'DXT3':
        a4 = (alpha * 15 + 127) // 255
        packed = (a4 << (4 * np.arange(16))).sum(1).astype('<u8')
        out = np.zeros(len(blocks), dtype=[('a', '<u8'), ('c', colour.dtype)])
        out['a'], out['c'] = packed, colour
        return out.tobytes()
    a0, a1 = alpha.max(1), alpha.min(1)
    levels = np.stack([a0, a1] + [((7 - k) * a0 + k * a1) // 7 for k in range(1, 7)], 1)
    index = np.abs(alpha[:, :, None] - levels[:, None, :]).argmin(-1)
    index[a0 == a1] = 0
    packed = (index.astype(np.uint64) << (3 * np.arange(16, dtype=np.uint64))).sum(1)
    head = np.zeros(len(blocks), dtype=[('a0', 'u1'), ('a1', 'u1'), ('bits', '<u2', 3), ('c', colour.dtype)])
    head['a0'], head['a1'] = a0, a1
    head['bits'] = np.stack([(packed >> np.uint64(16 * k)) & np.uint64(0xFFFF) for k in range(3)], 1)
    head['c'] = colour
    return head.tobytes()


def encode(image, width, height, fmt):
    rgba = np.asarray(image.convert('RGBA'))
    if rgba.shape[:2] != (height, width):
        raise SystemExit(f'image is {rgba.shape[1]}x{rgba.shape[0]}, the texture {width}x{height}')
    if fmt == 'ARGB':
        return rgba[..., [2, 1, 0, 3]].tobytes()
    return encode_dxt(rgba, fmt)


def decode(data, at, width, height, fmt):
    size = body_size(width, height, fmt)
    return Image.open(io.BytesIO(data[at:at + HEADER + size])).convert('RGBA')


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    command, path = argv[1], Path(argv[2])
    data = bytearray(path.read_bytes())
    found = textures(data)
    if command == 'list':
        for n, (at, w, h, fmt, mips) in enumerate(found):
            print(f'{n:3d} at {at:#x} {w}x{h} {fmt}' + (f' mips={mips}' if mips > 1 else ''))
        return 0
    if command == 'extract':
        out = Path(argv[3])
        out.mkdir(parents=True, exist_ok=True)
        for n, (at, w, h, fmt, _) in enumerate(found):
            decode(data, at, w, h, fmt).save(out / f'{path.name}_{n:02d}_{w}x{h}.png')
        print(f'{len(found)} textures to {out}')
        return 0
    if command == 'replace':
        output = Path(argv[argv.index('-o') + 1])
        for item in argv[3:argv.index('-o')]:
            n, png = item.split('=', 1)
            at, w, h, fmt, mips = found[int(n)]
            if mips > 1:
                raise SystemExit(f'texture {n} has mipmaps, not supported yet')
            body = encode(Image.open(png), w, h, fmt)
            assert len(body) == body_size(w, h, fmt)
            data[at + HEADER:at + HEADER + len(body)] = body
            print(f'texture {n} ({w}x{h} {fmt}) <- {png}')
        output.write_bytes(bytes(data))
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
