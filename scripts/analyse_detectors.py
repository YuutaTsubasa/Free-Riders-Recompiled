#!/usr/bin/env python3
"""Report what the title's race gesture detectors read from the player's body.

In a race the title fills a body record for each player from the Kinect
skeleton and runs about twenty-five gesture detectors on it (jump, crouch,
kick, lean, brake, stance...; see src/nui_race_hooks.cpp). This disassembles
each detector, and the Kinect manager update that fills the record, from the
decoded image on this machine (out/recomp/image-loader, made from the
player's own disc by the build steps), and annotates:

  * body+N   a read of the body record the detector asked its source for
             (every detector begins lwz r3,0(r4); lwz r11,0(r3);
             lwz r10,4(r11); bctrl, which returns the record in r3);
  * = value  the constants it compares against, read from the image where
             a lis/addi/lfs sequence points into it;
  * calls    the functions it branches to, one level of the small ones shown.

The report is for understanding the detectors, so a real body (a Kinect, a
webcam) can be fed to them as they expect; it quotes the game's code, so it
stays on the machine that made it (out/ is not committed).

  python scripts/analyse_detectors.py out/recomp/image-loader \\
      --functions out/recomp/diagnostic/ppc_func_mapping.cpp --output out/detectors.md
"""
import argparse
import csv
import re
import struct
import sys
from pathlib import Path

# The detectors, as nui_race_hooks.cpp names them, and the functions around them.
DETECTORS = [
    (0x822C9050, 'jump'),
    (0x822C8778, 'crouch'),
    (0x822CB840, 'charge'),
    (0x822C8650, 'arms up'),
    (0x822C9180, 'flying skill'),
    (0x822C9A80, 'board acceleration'),
    (0x822CAF48, 'board acceleration (second)'),
    (0x822CA6B0, 'side'),
    (0x822C9BF0, 'brake'),
    (0x822CB0B8, 'grab (poles, rails)'),
    (0x822CA518, 'kick dash'),
    (0x822CBD28, 'kick dash (second)'),
    (0x822CA810, 'power skill'),
    (0x822CBF30, 'power skill (second)'),
    (0x822CAC90, 'stance (regular / goofy)'),
    (0x822C9938, 'tricks'),
    (0x822CC2D0, 'item: hammer'),
    (0x822CC590, 'item: over throw'),
    (0x822CCD98, 'item: under swing'),
    (0x822CD068, 'item: under throw'),
    (0x822CDDB0, 'item: inflator'),
    (0x822CC8F0, 'item: shake'),
    (0x822CDEE0, 'steam wipe'),
    (0x822CD638, 'lever gear'),
    (0x822CD960, 'handle gear'),
    (0x822CD3F0, 'front curve gear'),
    (0x822CE118, 'swimming'),
    (0x822CB9B8, 'detector at 821A2484'),
    (0x822C8958, 'detector at 821A2344'),
    (0x822C6200, 'lean pair reader (+640 / +644)'),
]
CONTEXT = [
    (0x82438930, 'Kinect manager update (fills the body record)'),
    (0x822B72E0, 'race preparation ("On your Gear!")'),
    # The depth view: on the console it turns the sensor's depth image into
    # the +640 / +644 lean pair (and whatever else the crouch needs).
    (0x82439530, 'depth view update'),
    (0x824395E8, 'Kinect frame thread (waits for skeleton and depth frames)'),
]
MAPPING = re.compile(r'\{\s*0x([0-9A-Fa-f]+),\s*[A-Za-z_][A-Za-z0-9_]*\s*\},')

LOADS = {32: ('lwz', 'u32'), 33: ('lwzu', 'u32'), 34: ('lbz', 'u8'), 35: ('lbzu', 'u8'), 40: ('lhz', 'u16'),
         42: ('lha', 's16'), 48: ('lfs', 'f32'), 49: ('lfsu', 'f32'), 50: ('lfd', 'f64'), 51: ('lfdu', 'f64')}
STORES = {36: 'stw', 37: 'stwu', 38: 'stb', 39: 'stbu', 44: 'sth', 52: 'stfs', 53: 'stfsu', 54: 'stfd'}
FLOAT5 = {18: 'fdiv', 20: 'fsub', 21: 'fadd', 22: 'fsqrt', 23: 'fsel', 24: 'fres', 25: 'fmul', 26: 'frsqrte',
          28: 'fmsub', 29: 'fmadd', 30: 'fnmsub', 31: 'fnmadd'}
FLOAT10 = {0: 'fcmpu', 32: 'fcmpo', 40: 'fneg', 72: 'fmr', 136: 'fnabs', 264: 'fabs', 12: 'frsp', 14: 'fctiw',
           15: 'fctiwz', 846: 'fcfid', 815: 'fctidz'}
X31 = {266: 'add', 40: 'subf', 8: 'subfc', 235: 'mullw', 491: 'divw', 459: 'divwu', 28: 'and', 60: 'andc',
       444: 'or', 316: 'xor', 124: 'nor', 24: 'slw', 536: 'srw', 792: 'sraw', 824: 'srawi', 26: 'cntlzw',
       954: 'extsb', 922: 'extsh', 986: 'extsw', 104: 'neg', 0: 'cmpw', 32: 'cmplw', 23: 'lwzx', 87: 'lbzx',
       151: 'stwx', 535: 'lfsx', 663: 'stfsx', 339: 'mfspr', 467: 'mtspr', 19: 'mfcr', 144: 'mtcrf'}


def signed16(value):
    return value - 0x10000 if value & 0x8000 else value


class Image:
    """The decoded image: its bytes at their virtual addresses."""

    def __init__(self, dump):
        metadata = dict(line.split('\t') for line in (dump / 'image.tsv').read_text().splitlines())
        self.base = int(metadata['base'])
        self.data = (dump / 'image.bin').read_bytes()
        with (dump / 'sections.tsv').open() as stream:
            self.sections = [(int(s['address']), int(s['size'])) for s in csv.DictReader(stream, delimiter='\t')]

    @classmethod
    def from_bytes(cls, base, data):
        image = cls.__new__(cls)
        image.base, image.data, image.sections = base, data, [(base, len(data))]
        return image

    def contains(self, address, size=4):
        return self.base <= address and address + size <= self.base + len(self.data)

    def word(self, address):
        return struct.unpack_from('>I', self.data, address - self.base)[0]

    def value(self, address, kind):
        size = {'u8': 1, 'u16': 2, 's16': 2, 'u32': 4, 'f32': 4, 'f64': 8}[kind]
        if not self.contains(address, size):
            return None
        fmt = {'u8': '>B', 'u16': '>H', 's16': '>h', 'u32': '>I', 'f32': '>f', 'f64': '>d'}[kind]
        return struct.unpack_from(fmt, self.data, address - self.base)[0]


def describe(value, kind):
    if value is None:
        return None
    if kind in ('f32', 'f64'):
        return f'{value:.6g}'
    return f'0x{value:X}' if value > 9 else str(value)


def disassemble(image, start, end):
    """(address, text, facts) for each instruction; facts gathers what the
    summary needs: body reads, constants, callees."""
    constants = {}          # register -> known value (lis/addi/ori)
    body = {}               # register -> offset into the body record it points at
    pending_vtable = False  # lwz r10,4(r11) seen, so the next bctrl returns the record
    lines = []
    facts = {'body': {}, 'constants': [], 'calls': [], 'writes': {}}
    for address in range(start, end, 4):
        if not image.contains(address):
            break
        w = image.word(address)
        op = w >> 26
        d, a, b = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        simm, uimm = signed16(w & 0xFFFF), w & 0xFFFF
        text, note, written = f'.long 0x{w:08X}', '', None

        def base_note(offset, kind=None):
            if a in body:
                offset += body[a]
                facts['body'].setdefault(offset, set()).add(kind or '?')
                return f'body+{offset}'
            if a in constants:
                target = (constants[a] + offset) & 0xFFFFFFFF
                shown = describe(image.value(target, kind), kind) if kind else None
                if shown is not None:
                    facts['constants'].append((target, kind, shown))
                return f'[0x{target:08X}]' + (f' = {shown}' if shown is not None else '')
            return ''

        if op == 14:
            text = f'li r{d},{simm}' if a == 0 else f'addi r{d},r{a},{simm}'
            written = d
            if a == 0:
                constants[d] = simm & 0xFFFFFFFF
            elif a in constants:
                constants[d] = (constants[a] + simm) & 0xFFFFFFFF
                note = f'= 0x{constants[d]:08X}'
            else:
                constants.pop(d, None)
            if a != 0 and a in body:
                body[d] = body[a] + simm  # a pointer into the record
                note = f'= body+{body[d]}'
            else:
                body.pop(d, None)
        elif op == 15:
            text = f'lis r{d},0x{uimm:X}' if a == 0 else f'addis r{d},r{a},0x{uimm:X}'
            written = d
            if a == 0:
                constants[d] = (uimm << 16) & 0xFFFFFFFF
            else:
                constants.pop(d, None)
            body.pop(d, None)
        elif op == 24:
            text = f'ori r{a},r{d},0x{uimm:X}'
            if d in constants:
                constants[a] = constants[d] | uimm
            else:
                constants.pop(a, None)
            body.pop(a, None)
            written = None
        elif op in (10, 11):
            crf = (w >> 23) & 7
            text = (f'cmplwi cr{crf},r{a},{uimm}' if op == 10 else f'cmpwi cr{crf},r{a},{simm}')
        elif op == 7:
            text, written = f'mulli r{d},r{a},{simm}', d
        elif op == 28:
            text, written = f'andi. r{a},r{d},0x{uimm:X}', a
        elif op in LOADS:
            name, kind = LOADS[op]
            prefix = 'f' if name.startswith('lf') else 'r'
            text = f'{name} {prefix}{d},{simm}(r{a})'
            note = base_note(simm, kind)
            if prefix == 'r':
                written = d
                # The detectors' opening: the source's object, its vtable, vtable[1].
                if name == 'lwz' and simm == 4 and a == 11 and d == 10:
                    pending_vtable = True
        elif op in STORES:
            name = STORES[op]
            prefix = 'f' if name.startswith('stf') else 'r'
            text = f'{name} {prefix}{d},{simm}(r{a})'
            note = base_note(simm)
            if a in body:
                facts['writes'][simm] = True
        elif op == 18:
            li = w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            target = (li if w & 2 else address + li) & 0xFFFFFFFF
            text = ('bl' if w & 1 else 'b') + f' 0x{target:08X}'
            if w & 1:
                facts['calls'].append(target)
                for register in range(3, 13):
                    constants.pop(register, None)
                    body.pop(register, None)
        elif op == 16:
            bd = w & 0xFFFC
            if bd & 0x8000:
                bd -= 0x10000
            target = (bd if w & 2 else address + bd) & 0xFFFFFFFF
            text = f'bc{"l" if w & 1 else ""} {d},{a},0x{target:08X}'
        elif op == 19:
            xo = (w >> 1) & 0x3FF
            if xo == 16:
                text = 'blr' if d == 20 else f'bclr {d},{a}'
            elif xo == 528:
                text = 'bctrl' if (w & 1) and d == 20 else ('bctr' if d == 20 else f'bcctr {d},{a}')
                if w & 1:
                    for register in range(3, 13):
                        constants.pop(register, None)
                        body.pop(register, None)
                    if pending_vtable:
                        body[3] = 0
                        note = 'returns the body record in r3'
                        pending_vtable = False
            else:
                text = f'cr-op {xo}'
        elif op == 21:
            sh, mb, me = b, (w >> 6) & 31, (w >> 1) & 31
            text, written = f'rlwinm r{a},r{d},{sh},{mb},{me}', a
        elif op == 31:
            xo = (w >> 1) & 0x3FF
            name = X31.get(xo, f'x31.{xo}')
            if xo == 444 and d == b:
                text = f'mr r{a},r{d}'
                if d in body:
                    body[a] = body[d]
                else:
                    body.pop(a, None)
                if d in constants:
                    constants[a] = constants[d]
                else:
                    constants.pop(a, None)
                written = None
            elif xo in (339, 467):
                spr = ((w >> 16) & 31) | (((w >> 11) & 31) << 5)
                spr_name = {8: 'lr', 9: 'ctr'}.get(spr, str(spr))
                text = f'mf{spr_name} r{d}' if xo == 339 else f'mt{spr_name} r{d}'
                written = d if xo == 339 else None
            elif xo in (0, 32):
                text = f'{name} cr{(w >> 23) & 7},r{a},r{b}'
            else:
                text = f'{name} r{d},r{a},r{b}'
                written = a if xo in (28, 60, 444, 316, 124, 24, 536, 792, 824, 26, 954, 922, 986) else d
        elif op in (59, 63):
            xo5, xo10 = (w >> 1) & 31, (w >> 1) & 0x3FF
            if op == 63 and xo10 in FLOAT10:
                name = FLOAT10[xo10]
                text = f'{name} cr{(w >> 23) & 7},f{a},f{b}' if name.startswith('fcmp') else f'{name} f{d},f{b}'
            elif xo5 in FLOAT5:
                c = (w >> 6) & 31
                name = FLOAT5[xo5] + ('s' if op == 59 else '')
                text = f'{name} f{d},f{a},f{c if xo5 == 25 else b}' + (f',f{b}' if xo5 >= 28 or xo5 == 23 else '')
            else:
                text = f'fp{op}.{xo10}'
        elif op == 4:
            text = 'vmx'
        elif op in (58, 62):
            ds = signed16(w & 0xFFFC)
            text = f'{"ld" if op == 58 else "std"} r{d},{ds}(r{a})'
            written = d if op == 58 else None
            note = base_note(ds)
        if written is not None and op not in (14, 15):
            constants.pop(written, None)
            body.pop(written, None)
        lines.append((address, text, note))
        if text == 'blr' and address + 4 >= end:
            break
    return lines, facts


def function_ends(mapping):
    """Each function's end: the next function's start."""
    starts = sorted({int(address, 16) for address in MAPPING.findall(mapping)})
    return {start: following for start, following in zip(starts, starts[1:])}


def report(image, ends, functions, callee_limit=160):
    names = dict(DETECTORS + CONTEXT)
    out = ['# Race gesture detectors', '',
           'Made by scripts/analyse_detectors.py from this machine\'s decoded image. It quotes the game\'s',
           'code: keep it on this machine. `body+N` is a read of the body record the detector asked its',
           'source for; `= value` a constant read from the image.', '']
    summary = ['| Address | Detector | Body fields read | Constants | Calls |', '| --- | --- | --- | --- | --- |']
    sections, shown_callees = [], set()
    for address, name in functions:
        end = ends.get(address)
        if end is None:
            summary.append(f'| 0x{address:08X} | {name} | (not a function in the mapping) | | |')
            continue
        lines, facts = disassemble(image, address, end)
        fields = ', '.join(f'+{offset}' for offset in sorted(facts['body']))
        constants = ', '.join(sorted({shown for _, kind, shown in facts['constants'] if kind in ('f32', 'f64')},
                                     key=lambda text: float(text)))
        calls = ', '.join(f'0x{c:08X}' + (f' ({names[c]})' if c in names else '') for c in dict.fromkeys(facts['calls']))
        summary.append(f'| 0x{address:08X} | {name} | {fields or "-"} | {constants or "-"} | {calls or "-"} |')
        body = [f'## 0x{address:08X} {name}', '', f'{len(lines)} instructions.', '', '```']
        body += [f'{a:08X}  {t:<34}{("; " + n) if n else ""}' for a, t, n in lines]
        body += ['```', '']
        # One level of the small functions it calls: the joint and vector
        # helpers a detector leans on.
        for callee in dict.fromkeys(facts['calls']):
            callee_end = ends.get(callee)
            if callee in shown_callees or callee_end is None or (callee_end - callee) // 4 > callee_limit:
                continue
            shown_callees.add(callee)
            callee_lines, _ = disassemble(image, callee, callee_end)
            body += [f'### called: 0x{callee:08X}' + (f' ({names[callee]})' if callee in names else ''), '', '```']
            body += [f'{a:08X}  {t:<34}{("; " + n) if n else ""}' for a, t, n in callee_lines]
            body += ['```', '']
        sections += body
    return '\n'.join(out + summary + [''] + sections)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('dump', type=Path, help='the decoded image (out/recomp/image-loader)')
    parser.add_argument('--functions', type=Path,
                        help='ppc_func_mapping.cpp of the generated game (default: the first under out/recomp)')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not (args.dump / 'complete.txt').is_file():
        parser.error('image dump has no completion marker')
    mapping = args.functions or next(Path('out/recomp').glob('*/ppc_func_mapping.cpp'), None)
    if not mapping or not mapping.is_file():
        parser.error('no ppc_func_mapping.cpp found: pass --functions')
    image = Image(args.dump)
    text = report(image, function_ends(mapping.read_text()), DETECTORS + CONTEXT)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding='utf-8')
    print(f'{args.output}: {len(DETECTORS)} detectors and {len(CONTEXT)} related functions')


if __name__ == '__main__':
    sys.exit(main())
