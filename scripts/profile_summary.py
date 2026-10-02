#!/usr/bin/env python3
"""Where the main thread's time goes, from SFR_MAIN_PROFILE samples.

The host profiler (diagnostic_main.cpp) samples the main guest thread's
instruction pointer every millisecond and prints, at the end of the run,
one 'HOST_PROFILE rva=0x... count' line per address, relative to the
executable's base. The linker's map (sfr_cpu_diagnostic.map, beside the
executable) names the function and object file holding each address.

    python scripts/profile_summary.py out/bench/<run> [--map path] [--top 40]

reads every *.log in the directory that has HOST_PROFILE lines, prints a
table per category of code (the recompiled game, guest memory access, the
renderer, ...) and the hottest functions, and writes profile.md.
"""
import argparse
import bisect
import re
from pathlib import Path

SAMPLE = re.compile(r'^HOST_PROFILE rva=0x([0-9a-f]+) (\d+)')
# MSVC link.exe: " 0001:00000000       ?name@@...    0000000140001000 f   object.obj"
MSVC_SYMBOL = re.compile(r'^\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s+(?:f\s+)?(?:i\s+)?(\S+)\s*$', re.I)
MSVC_BASE = re.compile(r'Preferred load address is ([0-9a-f]+)', re.I)
# What the object files are, in the order they are tried.
CATEGORIES = (
    ('遊戲生成碼', re.compile(r'ppc_recomp|ppc_func_mapping', re.I)),
    ('客體記憶體存取', re.compile(r'guest_memory|vector_memory|memory_update|load_halfword|store_halfword|store_float', re.I)),
    ('執行許可與排程', re.compile(r'guest_execution|guest_threads|native_sync|critical_section', re.I)),
    ('畫圖', re.compile(r'guest_graphics|native_renderer|native_presentation|native_graphics|native_shaders|native_render|'
                       r'native_raster|native_formats|texture|plume|d3d12|vulkan|vma', re.I)),
    ('HLE 與診斷', re.compile(r'diagnostic_main|nui_|xex_module|virtual_memory|physical_memory|native_', re.I)),
    ('C/C++ 執行階段', re.compile(r'libcmt|libvcruntime|libucrt|msvcprt|msvcrt|vcruntime|ucrt|libcpmt', re.I)),
)
OUTSIDE = '執行檔以外（系統、驅動程式、等待中）'


def read_map(path):
    """Sorted [(rva, name, object)] from the linker map (link.exe's format,
    which lld-link /MAP writes too), public and static symbols alike."""
    text = Path(path).read_text(encoding='utf-8', errors='replace').splitlines()
    symbols, base = [], None
    for line in text:
        match = MSVC_BASE.search(line)
        if match:
            base = int(match.group(1), 16)
            continue
        match = MSVC_SYMBOL.match(line)
        if match and base is not None:
            name, address, obj = match.groups()
            address = int(address, 16)
            if address >= base:
                symbols.append((address - base, name, obj))
    symbols.sort()
    return symbols


def demangle(name):
    """A readable name for an MSVC-decorated one: ?draw@NativeRenderer@sfr@@... -> sfr::NativeRenderer::draw."""
    if not name.startswith('?') or name.startswith('?$') or name.startswith('??'):
        return name
    scope = name[1:].split('@@')[0].split('@')
    return '::'.join(reversed([part for part in scope if part]))


def category(obj):
    for title, pattern in CATEGORIES:
        if pattern.search(obj):
            return title
    return '其他'


def read_samples(directory):
    samples = {}
    for log in sorted(Path(directory).glob('*.log')):
        with open(log, encoding='utf-8', errors='replace') as lines:
            for line in lines:
                match = SAMPLE.match(line)
                if match:
                    rva = int(match.group(1), 16)
                    samples[rva] = samples.get(rva, 0) + int(match.group(2))
    return samples


def summarise(samples, symbols, top=40):
    addresses = [rva for rva, _, _ in symbols]
    last = symbols[-1][0] + 0x100000 if symbols else 0
    by_function, by_category = {}, {}
    for rva, count in samples.items():
        index = bisect.bisect_right(addresses, rva) - 1
        # Past the image (a system DLL, the driver) or before its first symbol.
        if index < 0 or rva > last or rva >= 1 << 32:
            key, where = (OUTSIDE, ''), OUTSIDE
        else:
            _, name, obj = symbols[index]
            key, where = (demangle(name), obj), category(obj)
        by_function[key] = by_function.get(key, 0) + count
        by_category[where] = by_category.get(where, 0) + count
    total = sum(samples.values()) or 1
    lines = [f'主執行緒取樣 {total} 次（每 1 ms 一次）', '', '| 類別 | 比例 |', '| --- | ---: |']
    for where, count in sorted(by_category.items(), key=lambda item: -item[1]):
        lines.append(f'| {where} | {100 * count / total:.1f}% |')
    lines += ['', '| 函式 | 目的檔 | 比例 |', '| --- | --- | ---: |']
    for (name, obj), count in sorted(by_function.items(), key=lambda item: -item[1])[:top]:
        lines.append(f'| `{name}` | {obj} | {100 * count / total:.2f}% |')
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('directory')
    parser.add_argument('--map', help='the linker map (default: sfr_cpu_diagnostic.map in the directory)')
    parser.add_argument('--top', type=int, default=40)
    args = parser.parse_args()
    samples = read_samples(args.directory)
    if not samples:
        print('No HOST_PROFILE samples (run the benchmark with -Configs profile).')
        return
    map_path = Path(args.map) if args.map else Path(args.directory) / 'sfr_cpu_diagnostic.map'
    symbols = read_map(map_path) if map_path.exists() else []
    if not symbols:
        print(f'No symbols read from {map_path}; every sample counts as outside the executable.')
    table = summarise(samples, symbols, args.top)
    print(table)
    (Path(args.directory) / 'profile.md').write_text(table + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
