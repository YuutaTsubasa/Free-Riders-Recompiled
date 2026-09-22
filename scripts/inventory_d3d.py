"""Inventory the statically linked Xbox D3D functions the game calls.

For every function in the D3D library range that game code calls directly,
record its size, call sites, whether it writes the GPU command buffer, the
device fields it touches through its first argument, its D3D-range callees,
whether the diagnostic runtime already handles it, and whether a boot
entered it (SFR_FUNCTION_TRACE). The result plans native (HLE) replacements in batches instead of
one boot stop at a time; it names nothing it cannot observe.
"""
import argparse
import bisect
import collections
import json
import re
from pathlib import Path

from analyse_switches import BCTR, BLR, TEXT_BASE, TEXT_SIZE, Image, branch_target

ROOT = Path(__file__).resolve().parents[1]
# The D3D library object code: every command-buffer writer lies inside it.
D3D_LOW, D3D_HIGH = 0x824E4000, 0x8250D000
COMMAND_SPACE = 0x824F8720  # makes room in the device command buffer (+48 write, +56 limit)
# __savegprlr/__restgprlr helpers keep argument registers intact.
SAVE_RESTORE_LOW, SAVE_RESTORE_HIGH = 0x82A56000, 0x82A58A00


def call_target(address, word):
    if word >> 26 == 18 and word & 1:
        offset = word & 0x03FFFFFC
        offset -= 0x04000000 if offset & 0x02000000 else 0
        return (offset if word & 2 else address + offset) & 0xFFFFFFFF
    return None


def device_fields(image, start, end):
    """Offsets loaded/stored through r3, or through a register copied from r3."""
    aliases, fields = {3}, collections.Counter()
    for address in range(start, end, 4):
        word = image.word(address)
        opcode, d, a = word >> 26, (word >> 21) & 31, (word >> 16) & 31
        if opcode == 31 and (word >> 1) & 0x3FF == 444 and d == (word >> 11) & 31 and d in aliases:
            aliases.add(a)  # mr a,d
            continue
        if opcode in (32, 34, 36, 38, 40, 42, 44, 48, 50, 52, 54) and a in aliases:
            offset = word & 0xFFFF
            fields[offset - 0x10000 if offset & 0x8000 else offset] += 1
        elif opcode in (58, 62) and a in aliases:
            offset = word & 0xFFFC
            fields[offset - 0x10000 if offset & 0x8000 else offset] += 1
        if opcode not in (36, 37, 38, 39, 44, 45, 52, 53, 54, 55, 62) and d in aliases and d != 3:
            aliases.discard(d)  # overwritten by something other than a copy of the device
        target = call_target(address, word)
        if target is not None and not SAVE_RESTORE_LOW <= target < SAVE_RESTORE_HIGH:
            aliases &= set(range(13, 32))  # volatile registers are lost across calls
    return fields


def handled_addresses():
    """Guest addresses named in the diagnostic runtime (native hooks and allowlists)."""
    found = set()
    for path in (ROOT / 'src').glob('*.cpp'):
        for match in re.finditer(r'0x(82[45][0-9A-Fa-f]{5})\b', path.read_text(encoding='utf-8', errors='replace')):
            found.add(int(match[1], 16))
    return found


def reached(function_trace):
    """Addresses from a SFR_FUNCTION_TRACE file written by the diagnostic."""
    if not function_trace:
        return set()
    return {int(line, 16) for line in Path(function_trace).read_text().split()}


def inventory(data, function_trace=None):
    image = Image(data)
    pdata = image.pdata()
    starts = sorted(pdata)
    calls, callers = collections.Counter(), collections.defaultdict(set)
    for address in range(TEXT_BASE, TEXT_BASE + TEXT_SIZE, 4):
        target = call_target(address, image.word(address))
        if target is not None and D3D_LOW <= target < D3D_HIGH and not D3D_LOW <= address < D3D_HIGH:
            calls[target] += 1
            index = bisect.bisect_right(starts, address) - 1
            callers[target].add(starts[index] if index >= 0 else address)
    handled, boot = handled_addresses(), reached(function_trace)
    rows = []
    for start in sorted(calls):
        end = pdata.get(start)
        if end is None:  # no .pdata: stop at the first return or indirect tail
            end = start
            while end < D3D_HIGH and image.word(end) not in (BLR, BCTR):
                end += 4
            end += 4
        callees, writes_commands, tail = set(), False, None
        for address in range(start, end, 4):
            word = image.word(address)
            target = call_target(address, word)
            if target == COMMAND_SPACE:
                writes_commands = True
            elif target is not None and D3D_LOW <= target < D3D_HIGH:
                callees.add(target)
            branch = branch_target(address, word)
            if word >> 26 == 18 and branch is not None and not start <= branch < end:
                tail = branch
        fields = device_fields(image, start, end)
        rows.append({
            'address': start, 'size': end - start, 'call_sites': calls[start], 'callers': len(callers[start]),
            # Reading the +48 write pointer means inline packet writes.
            'writes_commands': writes_commands or 48 in fields,
            'device_fields': sorted(fields), 'd3d_callees': sorted(callees), 'tail_call': tail,
            'handled': start in handled, 'reached': start in boot,
        })
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--function-trace', type=Path, help='SFR_FUNCTION_TRACE output of a boot')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    rows = inventory(args.image.read_bytes(), args.function_trace)
    args.output.write_text(json.dumps([{**row, 'address': hex(row['address']),
                                        'd3d_callees': [hex(x) for x in row['d3d_callees']],
                                        'tail_call': hex(row['tail_call']) if row['tail_call'] else None}
                                       for row in rows], indent=1) + '\n')
    summary = collections.Counter()
    for row in rows:
        summary['functions'] += 1
        summary['command writers'] += row['writes_commands']
        summary['handled'] += row['handled']
        summary['reached'] += row['reached']
        summary['reached, not handled'] += row['reached'] and not row['handled']
    print(dict(summary))


if __name__ == '__main__':
    main()
