"""Find bounded jump tables by emulating each dispatch block for every case index.

XenonAnalyse matches fixed instruction orders that this game's compiler does not
emit, so it finds no tables. Instead, for each `bctr` this finds the
`cmplwi crN,rX,N` / `bgt crN` (or `bgtlr crN`) bound, derives which registers
equal the index plus a constant at the compare (through `mr` / `addi` copies
in the same straight-line block), then concretely executes the block from the
compare to `bctr` for index = 0..N, reading table bytes from the decoded image.

Anything not modelled makes the candidate fail instead of guessing: an
instruction whose register effects are unknown, an unknown register feeding
the target, a table outside read-only image data, or a label outside the
.pdata function containing the `bctr`. Other paths joining the sequence are
recorded, not rejected: generation dispatches on the computed CTR target and
stops on any target outside the verified labels, so an unbounded index on a
joined path fails loudly instead of jumping somewhere unverified.
"""
import argparse
import bisect
import json
import struct
from pathlib import Path

IMAGE_BASE = 0x82000000
TEXT_BASE, TEXT_SIZE = 0x82210000, 0x8BC4EC
PDATA_BASE, PDATA_SIZE = 0x821D6C00, 188904
BCTR = 0x4E800420
GT = 1
MAX_BLOCK = 24  # instructions searched back from bctr for the bound

# X/XO-form opcode 31 extended opcodes, grouped by destination.
X31_XO_WRITES_RD = {266, 10, 138, 40, 8, 136, 104, 235, 75, 11, 491, 459, 233, 73, 9, 457, 489,
                    202, 234, 200, 232}  # 9-bit XO-form; bit 9 of the 10-bit field is OE
X31_X_WRITES_RD = {23, 87, 279, 343, 21, 341, 339, 19, 371, 20, 84, 534, 790}
X31_WRITES_RD_AND_RA = {55, 119, 311, 375, 53, 373}  # load with update
X31_WRITES_RA = {28, 60, 444, 412, 316, 124, 476, 284, 24, 536, 792, 824, 922, 954, 986, 26, 58,
                 27, 539, 794, 826, 827}
# Stores, FP/vector loads, cache and ordering operations: no GPR or CR result.
X31_NO_GPR = {151, 215, 407, 149, 663, 727, 983, 535, 599, 103, 359, 7, 39, 71, 135, 167, 231,
              519, 551, 647, 679, 711, 743, 775, 807, 839, 871, 278, 246, 54, 86, 470, 1014,
              467, 598, 854, 918, 662}
X31_NO_GPR_UPDATE = {183, 247, 439, 695, 759, 181}  # store with update writes rA
X31_COMPARES = {0, 32}
X31_STORE_CONDITIONAL = {150, 214}  # stwcx. / stdcx. write cr0


class Rejected(Exception):
    pass


def s16(value):
    return value - 0x10000 if value & 0x8000 else value


def rotl32(value, shift):
    value &= 0xFFFFFFFF
    return ((value << shift) | (value >> (32 - shift))) & 0xFFFFFFFF if shift else value


def mask32(mb, me):
    bits = 0
    for bit in range(32):
        if (mb <= bit <= me) if mb <= me else (bit >= mb or bit <= me):
            bits |= 1 << (31 - bit)
    return bits


def is_control_flow(word):
    return word >> 26 in (16, 17, 18, 19)


def branch_target(address, word):
    opcode = word >> 26
    if opcode == 18 and not word & 1:
        offset = word & 0x03FFFFFC
        offset -= 0x04000000 if offset & 0x02000000 else 0
        return (offset if word & 2 else address + offset) & 0xFFFFFFFF
    if opcode == 16 and not word & 1:
        return (s16(word & 0xFFFC) if word & 2 else address + s16(word & 0xFFFC)) & 0xFFFFFFFF
    return None


def effects(word):
    """Return (gprs written, cr fields written) or None when not modelled."""
    opcode, d, a = word >> 26, (word >> 21) & 31, (word >> 16) & 31
    rc = {0} if word & 1 else set()
    if opcode in (32, 34, 40, 42, 58):  # lwz lbz lhz lha ld/ldu/lwa
        return ({d, a} if opcode == 58 and word & 3 == 1 else {d}), set()
    if opcode in (33, 35, 41, 43):  # load with update
        return {d, a}, set()
    if opcode in (48, 50):  # lfs lfd
        return set(), set()
    if opcode in (49, 51):
        return {a}, set()
    if opcode in (36, 38, 44, 52, 54):  # stores
        return set(), set()
    if opcode in (37, 39, 45, 53, 55):  # store with update
        return {a}, set()
    if opcode == 62:  # std / stdu
        return ({a} if word & 3 == 1 else set()), set()
    if opcode in (7, 8, 14, 15):  # mulli subfic addi addis
        return {d}, set()
    if opcode in (12, 13):  # addic addic.
        return {d}, ({0} if opcode == 13 else set())
    if opcode in (24, 25, 26, 27):  # ori oris xori xoris
        return {a}, set()
    if opcode in (28, 29):  # andi. andis.
        return {a}, {0}
    if opcode in (20, 21, 23, 30):  # rlwimi rlwinm rlwnm rld*
        return {a}, rc
    if opcode in (10, 11):
        return set(), {d >> 2}
    if opcode in (59, 63):  # floating point; record form writes cr1
        if opcode == 63 and (word >> 1) & 0x3FF in (0, 32):  # fcmpu / fcmpo
            return set(), {d >> 2}
        return set(), ({1} if word & 1 else set())
    if opcode == 4:  # VMX; record-form compares write cr6, so assume they might
        return set(), {6}
    if opcode == 31:
        extended = (word >> 1) & 0x3FF
        if extended in X31_COMPARES:
            return set(), {d >> 2}
        if extended in X31_STORE_CONDITIONAL:
            return set(), {0}
        if extended in X31_WRITES_RA:
            return {a}, rc
        if extended in X31_WRITES_RD_AND_RA:
            return {d, a}, set()
        if extended in X31_NO_GPR:
            return set(), set()
        if extended in X31_NO_GPR_UPDATE:
            return {a}, set()
        if extended in X31_X_WRITES_RD:
            return {d}, set()
        if extended & 0x1FF in X31_XO_WRITES_RD:
            return {d}, rc
    return None


class Image:
    def __init__(self, data):
        self.data = data

    def word(self, address):
        return struct.unpack_from('>I', self.data, address - IMAGE_BASE)[0]

    def read(self, address, size):
        # Jump tables live in read-only .rdata or inline in .text.
        if not (IMAGE_BASE <= address and address + size <= TEXT_BASE + TEXT_SIZE):
            raise Rejected(f'table read outside read-only image at {address:#x}')
        offset = address - IMAGE_BASE
        return int.from_bytes(self.data[offset:offset + size], 'big')

    def pdata(self):
        functions = {}
        for index in range(PDATA_SIZE // 8):
            begin, info = struct.unpack_from('>II', self.data, PDATA_BASE - IMAGE_BASE + index * 8)
            if begin:
                functions[begin] = begin + ((info >> 8) & 0x3FFFFF) * 4
        return functions


def execute(image, word, registers):
    """Apply one instruction to concrete registers; unknown results are dropped."""
    opcode, d, a, b = word >> 26, (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
    base = lambda: registers[a] if a else 0
    if word & 0xFC1FFFFF == 0x7C0903A6:  # mtctr rS
        if d in registers:
            registers['ctr'] = registers[d]
        else:
            registers.pop('ctr', None)
        return
    try:
        if opcode == 15:
            registers[d] = (base() + (s16(word & 0xFFFF) << 16)) & 0xFFFFFFFF
            return
        if opcode == 14:
            registers[d] = (base() + s16(word & 0xFFFF)) & 0xFFFFFFFF
            return
        if opcode == 24:
            registers[a] = registers[d] | (word & 0xFFFF)
            return
        if opcode == 21 and not word & 1:
            registers[a] = rotl32(registers[d], b) & mask32((word >> 6) & 31, (word >> 1) & 31)
            return
        if opcode == 31 and not word & 1:
            extended = (word >> 1) & 0x3FF
            if extended in (23, 279, 87):
                size = {23: 4, 279: 2, 87: 1}[extended]
                registers[d] = image.read((base() + registers[b]) & 0xFFFFFFFF, size)
                return
            if extended == 266:
                registers[d] = (registers[a] + registers[b]) & 0xFFFFFFFF
                return
            if extended == 444:
                registers[a] = registers[d] | registers[b]
                return
    except KeyError:
        pass  # an input is unknown, so the result is unknown too
    written = effects(word)
    if written is None:
        raise Rejected(f'unmodelled instruction {word:08x}')
    for register in written[0]:
        registers.pop(register, None)


def index_relations(image, compare, register, targets):
    """Registers equal to index + offset at the compare, via mr/addi copies."""
    relations = {register: 0}
    written_after = set()
    address = compare - 4
    while address >= TEXT_BASE and compare - address <= 4 * MAX_BLOCK:
        if address + 4 in targets and address + 4 <= compare:
            break  # another path joins here
        word = image.word(address)
        if is_control_flow(word):
            break
        written = effects(word)
        if written is None:
            break
        opcode, d, a, b = word >> 26, (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
        if opcode == 31 and (word >> 1) & 0x3FF == 444 and d == b and not word & 1:  # mr a,d
            destination, source, offset = a, d, 0
        elif opcode == 14 and a:
            destination, source, offset = d, a, s16(word & 0xFFFF)
        else:
            destination = None
        if destination is not None and destination != source \
                and destination not in written_after and source not in written_after:
            # Neither side changes before the compare, so the copy relation still holds there.
            if destination in relations:
                relations.setdefault(source, relations[destination] - offset)
            elif source in relations:
                relations[destination] = relations[source] + offset
        written_after |= written[0]
        address -= 4
    return relations


def find_bound(image, bctr):
    for branch in range(bctr - 4, bctr - 4 * MAX_BLOCK, -4):
        word = image.word(branch)
        if not is_control_flow(word):
            continue
        opcode, bo, bi = word >> 26, (word >> 21) & 31, (word >> 16) & 31
        conditional_return = opcode == 19 and (word >> 1) & 0x3FF == 16 and not word & 1
        if not (opcode == 16 and not word & 3 or conditional_return):
            raise Rejected(f'control flow before bound at {branch:#x}')
        if bo != 12 or bi & 3 != GT:
            raise Rejected(f'nearest conditional branch at {branch:#x} is not bgt/bgtlr')
        default = None if conditional_return else branch_target(branch, word)
        field = bi >> 2
        for compare in range(branch - 4, branch - 4 * MAX_BLOCK, -4):
            candidate = image.word(compare)
            if candidate >> 26 == 10 and (candidate >> 23) & 7 == field and not (candidate >> 21) & 1:
                return compare, branch, (candidate >> 16) & 31, candidate & 0xFFFF, default
            written = effects(candidate)
            if is_control_flow(candidate) or written is None or field in written[1]:
                raise Rejected(f'no dominating cmplwi for branch at {branch:#x}')
        raise Rejected(f'no cmplwi within range of {branch:#x}')
    raise Rejected('no bounding branch')


def unbounded_absolute_table(image, bctr, function):
    """Labels of `lis r12; rlwinm r0,rX,2; addi r12,r12; lwzx r0,r12,r0; mtctr r0; bctr`
    whose index range is established by the callers, not a local compare.

    The table is read word by word while each entry is an aligned address
    inside the containing .pdata function. Dispatch on the computed CTR target
    stops on anything outside the list, so a shorter list cannot misdirect.
    """
    if function is None:
        raise Rejected('unbounded table outside a .pdata function')
    words = [image.word(bctr - 4 * i) for i in range(5, 0, -1)]
    lis, rlwinm, addi, lwzx, mtctr = words
    if not (lis >> 26 == 15 and (lis >> 16) & 31 == 0 and (lis >> 21) & 31 == 12 and
            rlwinm >> 26 == 21 and (rlwinm >> 16) & 31 == 0 and (rlwinm >> 11) & 31 == 2 and
            (rlwinm >> 6) & 31 == 0 and (rlwinm >> 1) & 31 == 29 and
            addi >> 26 == 14 and (addi >> 21) & 31 == 12 and (addi >> 16) & 31 == 12 and
            lwzx == 0x7C0C002E and mtctr == 0x7C0903A6):
        raise Rejected('no bounding branch')
    table = ((lis & 0xFFFF) << 16) + s16(addi & 0xFFFF) & 0xFFFFFFFF
    labels = []
    while len(labels) < 256:
        entry = image.read(table + 4 * len(labels), 4)
        if entry % 4 or not function[0] <= entry < function[1]:
            break
        labels.append(entry)
    if len(labels) < 2:
        raise Rejected('no bounding branch')
    return (rlwinm >> 21) & 31, labels


def analyse(data):
    image = Image(data)
    pdata = image.pdata()
    starts = sorted(pdata)
    targets, bctrs = set(), []
    for address in range(TEXT_BASE, TEXT_BASE + TEXT_SIZE, 4):
        word = image.word(address)
        target = branch_target(address, word)
        if target is not None:
            targets.add(target)
        if word == BCTR:
            bctrs.append(address)

    def function_of(address):
        index = bisect.bisect_right(starts, address) - 1
        if index >= 0 and address < pdata[starts[index]]:
            return starts[index], pdata[starts[index]]
        return None

    tables, rejected = [], []
    for bctr in bctrs:
        try:
            try:
                compare, branch, register, limit, default = find_bound(image, bctr)
            except Rejected as unbounded:
                if not str(unbounded).startswith(('no bounding branch', 'control flow before bound')):
                    raise
                register, labels = unbounded_absolute_table(image, bctr, function_of(bctr))
                tables.append({'base': bctr, 'r': register, 'default': None, 'labels': labels,
                               'bound': bctr, 'joined_at': [], 'unbounded': True,
                               'pdata_function': function_of(bctr)[0]})
                continue
            # Other paths may join after the compare. Their index is unbounded
            # here, which is safe only because generation dispatches on the
            # computed CTR target and stops on any target not listed below.
            joined = sorted(t for t in targets if compare < t <= bctr)
            relations = index_relations(image, compare, register, targets)
            labels, final_states = [], []
            for index in range(limit + 1):
                registers = {r: (index + offset) & 0xFFFFFFFF for r, offset in relations.items()}
                for address in range(compare + 4, bctr, 4):
                    if address != branch:
                        execute(image, image.word(address), registers)
                if 'ctr' not in registers:
                    raise Rejected('dispatch target depends on an unknown register')
                labels.append(registers['ctr'])
                final_states.append(registers)
            # XenonRecomp needs an index register to emit labels; generation
            # replaces its index switch with a CTR-target dispatch.
            holders = [r for r in range(32)
                       if all(state.get(r) == index for index, state in enumerate(final_states))]
            switch_register = register if register in holders or not holders else holders[0]
            function = function_of(bctr)
            for label in labels + ([default] if default is not None else []):
                if label % 4 or not TEXT_BASE <= label < TEXT_BASE + TEXT_SIZE:
                    raise Rejected(f'label {label:#x} is not an aligned .text address')
                if function and not function[0] <= label < function[1]:
                    raise Rejected(f'label {label:#x} leaves .pdata function {function[0]:#x}')
            tables.append({'base': bctr, 'r': switch_register, 'default': default, 'labels': labels,
                           'bound': compare, 'joined_at': joined,
                           'pdata_function': function[0] if function else None})
        except Rejected as error:
            rejected.append({'bctr': bctr, 'reason': str(error)})
    return tables, rejected, len(bctrs)


BLR = 0x4E800020


def function_extent(image, start, limit, tables):
    """Return the end of code reachable from start without leaving [start, limit).

    Follows fallthrough, local branches and jump table labels. Branches to
    addresses outside the range are calls/tail calls and are not followed.
    """
    pending, seen, end = [start], set(), start
    while pending:
        address = pending.pop()
        while address not in seen:
            if not start <= address < limit:
                raise Rejected(f'function {start:#x} falls through past {limit:#x}')
            seen.add(address)
            end = max(end, address + 4)
            word = image.word(address)
            if word == 0:
                raise Rejected(f'function {start:#x} reaches padding at {address:#x}')
            if word == BLR:
                break
            if word == BCTR:
                if address in tables:
                    pending.extend(tables[address])
                break
            target = branch_target(address, word)
            if word >> 26 == 18:
                if word & 1:  # bl returns here
                    address += 4
                    continue
                if not start <= target < limit:
                    break  # tail call
                address = target
                continue
            if target is not None and start <= target < limit:
                pending.append(target)
            address += 4
    return end


def table_functions(data, tables):
    """Give each jump table outside .pdata the function that contains it."""
    image = Image(data)
    pdata = image.pdata()
    starts = sorted(pdata)
    calls = set()
    for address in range(TEXT_BASE, TEXT_BASE + TEXT_SIZE, 4):
        word = image.word(address)
        if word >> 26 == 18 and word & 1:
            offset = word & 0x03FFFFFC
            offset -= 0x04000000 if offset & 0x02000000 else 0
            calls.add((offset if word & 2 else address + offset) & 0xFFFFFFFF)
    labels = {table['base']: table['labels'] for table in tables}
    boundary = lambda address: image.word(address - 4) in (0, BLR, BCTR) or (
        image.word(address - 4) >> 26 == 18 and not image.word(address - 4) & 1)
    functions, rejected = {}, []
    for table in sorted(tables, key=lambda item: item['base']):
        bctr = table['base']
        if table['pdata_function'] is not None or any(f <= bctr < e for f, (e, _) in functions.items()):
            continue
        index = bisect.bisect_right(starts, bctr)
        limit = starts[index] if index < len(starts) else TEXT_BASE + TEXT_SIZE
        floor = starts[index - 1] if index else TEXT_BASE
        floor = pdata[floor] if index and pdata[floor] <= bctr else floor
        # Prefer a direct call target; otherwise the nearest code boundary.
        candidates = [a for a in range(bctr, floor - 4, -4) if a in calls]
        candidates += [a for a in range(bctr, floor - 4, -4) if a not in calls and boundary(a)]
        for method in ('bl-target', 'boundary'):
            chosen = None
            for start in candidates:
                if (start in calls) != (method == 'bl-target'):
                    continue
                try:
                    end = function_extent(image, start, limit, labels)
                except Rejected:
                    continue
                if start <= bctr < end:
                    chosen = (start, end)
                    break
            if chosen:
                functions[chosen[0]] = (chosen[1], method)
                break
        else:
            rejected.append({'bctr': hex(bctr), 'reason': 'no reachable function start contains the table'})
    return [{'address': start, 'size': end - start, 'method': method}
            for start, (end, method) in sorted(functions.items())], rejected


def to_toml(tables):
    lines = ['# Generated by scripts/analyse_switches.py from the verified decoded image.']
    for table in tables:
        lines += ['', '[[switch]]', f'base = 0x{table["base"]:X}', f'r = {table["r"]}']
        if table['default'] is not None:
            lines.append(f'default = 0x{table["default"]:X}')
        lines.append('labels = [')
        lines += [f'    0x{label:X},' for label in table['labels']]
        lines.append(']')
    return '\n'.join(lines) + '\n'


def report(tables, rejected, count):
    hexed = lambda value: hex(value) if value is not None else None
    return {'bctr_sites': count,
            'tables': [{**t, 'base': hex(t['base']), 'default': hexed(t['default']), 'bound': hex(t['bound']),
                        'labels': [hex(x) for x in t['labels']], 'joined_at': [hex(x) for x in t['joined_at']],
                        'pdata_function': hexed(t['pdata_function'])}
                       for t in tables],
            'rejected': [{**r, 'bctr': hex(r['bctr'])} for r in rejected]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path, help='decoded image.bin')
    parser.add_argument('--output', required=True, type=Path, help='switch table TOML')
    parser.add_argument('--report', type=Path, help='JSON listing accepted and rejected candidates')
    args = parser.parse_args()
    tables, rejected, count = analyse(args.image.read_bytes())
    args.output.write_text(to_toml(tables))
    if args.report:
        args.report.write_text(json.dumps(report(tables, rejected, count), indent=1) + '\n')
    print(f'{len(tables)} jump tables from {count} bctr sites; {len(rejected)} not jump tables or rejected')


if __name__ == '__main__':
    main()
