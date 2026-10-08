#!/usr/bin/env python3
"""Read and rewrite the text of Free Riders' font files (gxf, gxfr, pauseF, resiF, FNT_S, adv).

    python scripts/sfr_font_text.py export <file> <strings.json>
    python scripts/sfr_font_text.py build <file> <strings.json> <font> <output> [--size N]
    python scripts/sfr_font_text.py check <file>   (rewrites the file unchanged: must be identical)

The files are the decompressed ones (scripts/unpack_assets.py); a mod can ship
them uncompressed. Each holds its own font: a "pack" whose entries are

- FONTDATF: a 60-byte header (glyph count at +16, cells per page at +20,
  pages at +24, page and last-page size at +26..+32, rows per page at +36,
  cell 23 x 34 at +42), then 16 bytes a glyph: ink width, left bearing,
  UTF-16 code, glyph number, right spacing (big-endian), then 4 bytes;
- the glyph pages: u16 count, u16 1, u32 offsets, u32 sizes, a 0x11 byte and a
  name for each, then each page as a DDS (A8R8G8B8, 512 wide). Glyph n sits in
  cell n of 22 x (page height / 34) cells of 23 x 34 pixels;
- FONTSTLB: "FONTSTLB", 8 header bytes, the string count, offsets from the
  chunk's start, then each string as big-endian 32-bit glyph numbers ending
  in 0x01000000 (0x01000002 is a line break, 0x01000013 / 0x01000014 open and
  close a reading shown above the text).

adv files are a version-2 pack: two u16 counts that add up, offsets from +20,
and pictures and layouts before the three font entries, which build keeps.

In strings.json a string is text with "\\n" for a line break and
"[base|reading]" for a reading. build keeps the glyphs the file already has
for every character it already draws (Latin, kana, punctuation) and draws the
others -- and every CJK ideograph the font has, so a translation gets one
consistent style -- with the given font. Nothing here contains game data.
"""
import json
import struct
import sys
from pathlib import Path

END, NEWLINE, RUBY_OPEN, RUBY_CLOSE = 0x01000000, 0x01000002, 0x01000013, 0x01000014
CELL_W, CELL_H, PER_ROW, PAGE_W = 23, 34, 22, 512


def u32(d, at):
    return struct.unpack('>I', d[at:at + 4])[0]


def pack_layout(d):
    """(offset table position, entries) of a pack. Version 1 counts its entries
    in the u16 at +8 and lists the offsets from +16; version 2 (adv) has two
    u16 counts that add up, and lists them from +20."""
    if d[:4] != b'pack':
        raise ValueError('not a pack')
    if d[5] == 1:
        n, table = struct.unpack('>H', d[8:10])[0], 16
    elif d[5] == 2:
        n, table = sum(struct.unpack('>HH', d[8:12])), 20
    else:
        raise ValueError('pack version %d' % d[5])
    offsets = struct.unpack('>%dI' % (n + 1), d[table:table + 4 * (n + 1)])
    return table, [(offsets[i], offsets[i + 1]) for i in range(n)]


def pack_entries(d):
    return pack_layout(d)[1]


class FontFile:
    def __init__(self, data):
        self.data = data
        table, entries = pack_layout(data)
        tags = [data[a:a + 8] for a, _ in entries]
        if b'FONTDATF' not in tags:
            raise ValueError('no font in this pack')
        # FONTDATF, the glyph pages and FONTSTLB follow each other; other
        # entries (adv's pictures and layouts) are kept as they are.
        self.font_at = tags.index(b'FONTDATF')
        if tags[self.font_at + 2] != b'FONTSTLB':
            raise ValueError('unexpected font entries')
        self.table = table
        self.entries = [data[a:b] for a, b in entries]
        (fa, fb), (ta, tb), (sa, sb) = entries[self.font_at:self.font_at + 3]
        self.font_header = data[fa:fa + 60]
        count = u32(data, fa + 16)
        self.glyphs = [dict(zip(('width', 'left', 'code', 'number', 'right'),
                                struct.unpack('>iiHHi', data[fa + 60 + 16 * i:fa + 76 + 16 * i])))
                       for i in range(count)]
        self.font_trailer = data[fa + 60 + 16 * count:fb]
        self.texture = data[ta:tb]
        self.strings_header = data[sa + 8:sa + 16]
        self.strings = read_strings(data, sa, sb)
        self.pack_header = data[:table]

    def pages(self):
        """The glyph pages as (name, height, 512 x height x 4 bytes)."""
        t = self.texture
        count = struct.unpack('>H', t[0:2])[0]
        offsets = struct.unpack('>%dI' % count, t[4:4 + 4 * count])
        sizes = struct.unpack('>%dI' % count, t[4 + 4 * count:4 + 8 * count])
        names_at = 4 + 8 * count + count
        names = t[names_at:offsets[0]].split(b'\0')[:count]
        out = []
        for name, offset, size in zip(names, offsets, sizes):
            dds = t[offset:offset + size]
            height, width = struct.unpack('<II', dds[12:20])
            assert dds[:4] == b'DDS ' and width == PAGE_W, (dds[:4], width)
            out.append((name.decode(), height, dds[128:128 + width * height * 4], dds[:128]))
        return out


def read_strings(d, start, end):
    count = u32(d, start + 16)
    offsets = struct.unpack('>%dI' % count, d[start + 20:start + 20 + 4 * count])
    strings = []
    for offset in offsets:
        at, values = start + offset, []
        while at + 4 <= end:
            v = u32(d, at)
            at += 4
            if v == END:
                break
            values.append(v)
        strings.append(values)
    return strings


def to_text(values, code_of):
    out = []
    for v in values:
        if v == NEWLINE:
            out.append('\n')
        elif v == RUBY_OPEN:
            out.append('|')
        elif v == RUBY_CLOSE:
            out.append(']')
        elif v >> 24:
            out.append('{%x}' % (v & 0xFFFFFF))
        else:
            out.append(chr(code_of[v]))
    text = ''.join(out)
    # base|reading] -> [base|reading]: the base is the run of ideographs before.
    result, i = '', 0
    while i < len(text):
        bar = text.find('|', i)
        if bar < 0:
            result += text[i:]
            break
        close = text.index(']', bar)
        base_start = bar
        while base_start > i and is_ideograph(text[base_start - 1]):
            base_start -= 1
        result += text[i:base_start] + '[' + text[base_start:bar] + '|' + text[bar + 1:close] + ']'
        i = close + 1
    return result


def is_ideograph(ch):
    o = ord(ch)
    return 0x3400 <= o <= 0x9FFF or 0xF900 <= o <= 0xFAFF or ch in '々〆〇'


def parse_text(text):
    """Text back to a list of characters and control values."""
    out, i = [], 0
    while i < len(text):
        ch = text[i]
        if ch == '\n':
            out.append(NEWLINE)
        elif ch == '[' and '|' in text[i:] and ']' in text[i:]:
            bar, close = text.index('|', i), text.index(']', i)
            out.extend(text[i + 1:bar])
            out.append(RUBY_OPEN)
            out.extend(text[bar + 1:close])
            out.append(RUBY_CLOSE)
            i = close + 1
            continue
        elif ch == '{' and '}' in text[i:]:
            close = text.index('}', i)
            out.append(0x01000000 | int(text[i + 1:close], 16))
            i = close + 1
            continue
        else:
            out.append(ch)
        i += 1
    return out


def serialize(font, glyphs, pages, strings, page_names, same_pages=False):
    """The pack from glyph records, page pixels (height, bytes) and value
    lists. same_pages keeps the font header's page fields (advE carries an
    empty page its font does not count)."""
    full_h = pages[0][0] if len(pages) > 1 else pages[-1][0]
    header = bytearray(font.font_header)
    struct.pack_into('>I', header, 16, len(glyphs))
    if not same_pages:
        struct.pack_into('>I', header, 20, (full_h // CELL_H) * PER_ROW)
        struct.pack_into('>5H', header, 24, len(pages), PAGE_W, full_h, PAGE_W, pages[-1][0])
        struct.pack_into('>H', header, 36, full_h // CELL_H)
    fontdat = bytearray(header)
    for g in glyphs:
        fontdat += struct.pack('>iiHHi', g['width'], g['left'], g['code'], g['number'], g['right'])
    fontdat += font.font_trailer

    count = len(pages)
    names = b''.join(n.encode() + b'\0' for n in page_names)
    head_size = 4 + 8 * count + count + len(names)
    # Where the originals put the first page (the game reads the offsets).
    first = 64 if head_size <= 64 else (head_size + 63) // 64 * 64 + 64
    dds_blobs = []
    template = font.pages()[0][3]
    for height, pixels in pages:
        dds = bytearray(template)
        struct.pack_into('<II', dds, 12, height, PAGE_W)
        dds_blobs.append(bytes(dds) + pixels)
    offsets, at = [], first
    for blob in dds_blobs:
        offsets.append(at)
        at += len(blob)
    texture = bytearray(struct.pack('>HH', count, 1))
    texture += struct.pack('>%dI' % count, *offsets)
    texture += struct.pack('>%dI' % count, *[len(b) for b in dds_blobs])
    texture += b'\x11' * count + names
    texture += b'\0' * (first - len(texture))
    for blob in dds_blobs:
        texture += blob

    body = bytearray()
    offsets = []
    table = 20 + 4 * len(strings)
    for values in strings:
        offsets.append(table + len(body))
        body += struct.pack('>%dI' % (len(values) + 1), *values, END)
    stlb = b'FONTSTLB' + font.strings_header + struct.pack('>I', len(strings))
    stlb += struct.pack('>%dI' % len(strings), *offsets) + body

    entries = list(font.entries)
    entries[font.font_at:font.font_at + 3] = [bytes(fontdat), bytes(texture), bytes(stlb)]
    if font.font_at + 3 == len(entries):
        entries[-1] += bytes(-(font.table + 4 * (len(entries) + 1) + sum(map(len, entries))) % 16)
    offsets, at = [], font.table + 4 * (len(entries) + 1)
    for entry in entries:
        offsets.append(at)
        at += len(entry)
    offsets.append(at)
    return bytes(font.pack_header) + struct.pack('>%dI' % len(offsets), *offsets) + b''.join(entries)


def original_layout(font):
    """The file's own glyphs, pages and strings, serialized again (for check)."""
    pages = [(h, px) for _, h, px, _ in font.pages()]
    return serialize(font, font.glyphs, pages, font.strings, [n for n, _, _, _ in font.pages()], same_pages=True)


def page_heights(cells):
    """Pages for this many cells: full 512 x 512 pages, the last one as small as fits."""
    per_full = (512 // CELL_H) * PER_ROW
    heights = []
    while cells > per_full:
        heights.append(512)
        cells -= per_full
    rows = -(-cells // PER_ROW)
    heights.append(next(h for h in (128, 256, 512) if h // CELL_H >= rows))
    return heights


def build(font, texts, font_path, size):
    from PIL import Image, ImageDraw, ImageFont
    import numpy as np

    old_pages = font.pages()
    old_full = old_pages[0][1] if len(old_pages) > 1 else old_pages[-1][1]
    per_old_page = (old_full // CELL_H) * PER_ROW
    old_pixels = [np.frombuffer(px, np.uint8).reshape(h, PAGE_W, 4) for _, h, px, _ in old_pages]
    old_by_code = {g['code']: g for g in font.glyphs}

    sequences = [parse_text(t) for t in texts]
    chars = sorted({c for seq in sequences for c in seq if isinstance(c, str)} | {' '})
    code_number = {c: i for i, c in enumerate(chars)}

    ttf = ImageFont.truetype(font_path, size)
    from fontTools.ttLib import TTFont
    font_has = set(TTFont(font_path, fontNumber=0).getBestCmap())
    try:
        ttf.set_variation_by_axes([700])  # as heavy as the game's own glyphs
    except Exception:
        pass
    heights = page_heights(len(chars))
    per_full = (512 // CELL_H) * PER_ROW
    pages = [np.zeros((h, PAGE_W, 4), np.uint8) for h in heights]
    for p in pages:
        p[..., :3] = 255
    glyphs = []
    for number, ch in enumerate(chars):
        page, cell = divmod(number, per_full)
        row, col = divmod(cell, PER_ROW)
        y0, x0 = row * CELL_H, col * CELL_W
        old = old_by_code.get(ord(ch))
        # The file's own glyph, unless it is an ideograph the font draws.
        if old is not None and (not is_ideograph(ch) or ord(ch) not in font_has):
            op, oc = divmod(old['number'], per_old_page)
            orow, ocol = divmod(oc, PER_ROW)
            pages[page][y0:y0 + CELL_H, x0:x0 + CELL_W] = \
                old_pixels[op][orow * CELL_H:(orow + 1) * CELL_H, ocol * CELL_W:(ocol + 1) * CELL_W]
            glyphs.append(dict(old, number=number))
            continue
        if ch != ' ' and ord(ch) not in font_has:
            raise SystemExit(f'{ch!r} (U+{ord(ch):04X}): neither the file nor the font draws it')
        image = Image.new('L', (CELL_W, CELL_H), 0)
        draw = ImageDraw.Draw(image)
        if ch != ' ':
            left, top, right, bottom = draw.textbbox((0, 0), ch, font=ttf, anchor='ls')
            draw.text((1 - left, 27), ch, font=ttf, fill=255, anchor='ls')
        alpha = np.array(image)
        alpha = np.where(alpha > 0, (alpha // 16) * 16 + 15, 0).astype(np.uint8)
        pages[page][y0:y0 + CELL_H, x0:x0 + CELL_W, 3] = alpha
        ink = np.where(alpha.max(0) > 0)[0]
        width = int(ink.max()) if len(ink) else 1
        advance = size if is_ideograph(ch) or ord(ch) >= 0x3000 else width + 3
        left = max(0, (advance - width) // 2) if is_ideograph(ch) else 1
        glyphs.append(dict(width=width, left=left, code=ord(ch), number=number,
                           right=max(0, advance - width - left)))

    strings = [[code_number[c] if isinstance(c, str) else c for c in seq] for seq in sequences]
    base = old_pages[0][0].rsplit('_', 1)[0]
    names = ['%s_%02d' % (base, i) for i in range(len(pages))]
    return serialize(font, glyphs, [(h, p.tobytes()) for h, p in zip(heights, pages)], strings, names)


def find_chunks(d):
    """FONTDATF and FONTSTLB anywhere in a file (adv nests them)."""
    fa = d.find(b'FONTDATF')
    sa = d.find(b'FONTSTLB')
    count = u32(d, fa + 16)
    glyphs = {struct.unpack('>H', d[fa + 70 + 16 * i:fa + 72 + 16 * i])[0]:
              struct.unpack('>H', d[fa + 68 + 16 * i:fa + 70 + 16 * i])[0] for i in range(count)}
    return glyphs, read_strings(d, sa, len(d))


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    command, path = argv[1], Path(argv[2])
    data = path.read_bytes()
    if command == 'export':
        code_of, strings = find_chunks(data)
        texts = [to_text(s, code_of) for s in strings]
        Path(argv[3]).write_text(json.dumps(texts, ensure_ascii=False, indent=1), encoding='utf-8')
        print(f'{path.name}: {len(texts)} strings, {len(code_of)} glyphs')
        return 0
    if command == 'check':
        font = FontFile(data)
        again = original_layout(font)
        print(f'{path.name}: {len(font.glyphs)} glyphs, {len(font.strings)} strings, '
              f'rewritten {"identical" if again == data else "DIFFERENT"} ({len(again)} / {len(data)} bytes)')
        return 0 if again == data else 1
    if command == 'build':
        font = FontFile(data)
        texts = json.loads(Path(argv[3]).read_text(encoding='utf-8'))
        if len(texts) != len(font.strings):
            raise SystemExit(f'{len(texts)} strings, the file has {len(font.strings)}')
        size = int(argv[argv.index('--size') + 1]) if '--size' in argv else 20
        out = build(font, texts, argv[4], size)
        Path(argv[5]).write_bytes(out)
        print(f'{argv[5]}: {len(out)} bytes')
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
