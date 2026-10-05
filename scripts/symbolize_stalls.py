"""Names the STALL_STACK addresses of a game.log (SFR_STALL_MS) with the
executable's linker map (link with /MAP: -DCMAKE_EXE_LINKER_FLAGS=/MAP).

Usage: python scripts/symbolize_stalls.py game.log sfr_cpu_diagnostic.map
Prints each sample with the function each exe+0x... offset falls in, then how
often each function was the innermost one in the executable.
"""
import bisect
import collections
import re
import sys


def load_map(path):
    base, symbols = 0x140000000, []
    with open(path, errors='ignore') as stream:
        for line in stream:
            preferred = re.search(r'Preferred load address is ([0-9a-fA-F]+)', line)
            if preferred:
                base = int(preferred.group(1), 16)
            match = re.match(r'\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s', line)
            if match and int(match.group(2), 16):
                symbols.append((int(match.group(2), 16) - base, match.group(1)))
    symbols.sort()
    return [rva for rva, _ in symbols], [name for _, name in symbols]


def name(rvas, names, rva):
    i = bisect.bisect_right(rvas, rva) - 1
    return f'{names[i]}+0x{rva - rvas[i]:x}' if i >= 0 else f'exe+0x{rva:x}'


def main():
    log, map_path = sys.argv[1], sys.argv[2]
    rvas, names = load_map(map_path)
    innermost = collections.Counter()
    with open(log, errors='ignore') as stream:
        for line in stream:
            if not line.startswith('STALL_STACK'):
                continue
            head = ' '.join(line.split()[:4])
            frames = [name(rvas, names, int(m, 16)) for m in re.findall(r'exe\+0x([0-9a-f]+)', line)]
            print(head, ' <- '.join(frames[:8]))
            if frames:
                innermost[re.sub(r'\+0x[0-9a-f]+$', '', frames[0])] += 1
    print('\ninnermost functions:')
    for function, count in innermost.most_common(20):
        print(f'{count:5d} {function}')


if __name__ == '__main__':
    main()
