#!/usr/bin/env python3
"""Makes a copy of a benchmark folder that is safe to send to someone else.

    py scripts\\anonymize_benchmark.py out\\bench\\20260930-151014
    py scripts\\anonymize_benchmark.py out\\bench\\20260930-151014 --also MYPC --keep-screenshots

Writes <folder>-shareable beside it (and a .zip of it) and leaves the original
alone. No file it makes is over 29 MB (--limit-mb): a result that does not fit
is split into <folder>-shareable-part1of3.zip, -part2of3.zip, ... whole files
to a part, and all the parts are unpacked into one folder on the other side.
A single file that is itself over the limit goes in pieces, name.001, name.002,
...; join them (copy /b name.001+name.002 name) before use. What the logs hold that says who ran them, and what it becomes:

- the Windows user name in every path (C:\\Users\\<name>\\...): C:\\Users\\USER\\...
  (the same for /home/<name> and /Users/<name>);
- the folder the generated code was found in (info.txt "generated="): removed;
- the language and country the game was told (NATIVE_USER_LANGUAGE/COUNTRY):
  the values are replaced;
- the save-profile id folder the game wrote to (E000 and twelve hex digits):
  E000XXXXXXXXXXXX;
- anything you name with --also (a computer name, your own name): XXXX;
- screenshots (*.bmp): left out unless --keep-screenshots, as they show whatever
  was on the screen.

Kept, because a comparison needs it: the commit, the CPU and GPU names, the
Windows version and every timing. Read the result before sending it: this
removes what the tool knows to look for, not everything that could identify you.
"""
import argparse
import re
import shutil
import sys
import zipfile
from pathlib import Path

# Decimal megabytes, so a file under the cap is under it however a service counts.
LIMIT_MB = 29
# What a zip entry and its record in the directory cost beyond the data, with room to spare.
ENTRY_OVERHEAD = 400
TEXT = {'.log', '.txt', '.md', '.json', '.out', '.csv', '.map'}


def patterns(also):
    rules = [
        # A Windows user name may contain spaces: it ends at the next slash.
        (rb'(?i)([A-Z]:[\\/]Users[\\/])[^\\/"\'\r\n]+', rb'\1USER'),
        (rb'(/home/|/Users/)[^/\s"\']+', rb'\1USER'),
        (rb'(?m)^generated=.*$', rb'generated=<removed>'),
        (rb'windows_langid=0x[0-9A-Fa-f]+ xbox_language=\d+', rb'windows_langid=0x0 xbox_language=0'),
        (rb'(NATIVE_USER_COUNTRY source=\S+ )iso=\S+ xbox_country=\d+', rb'\1iso=XX xbox_country=0'),
        (rb'E000[0-9A-Fa-f]{12}', rb'E000XXXXXXXXXXXX'),
    ]
    for word in also:
        rules.append((re.escape(word.encode()), b'XXXX'))
    return [(re.compile(pattern), replacement) for pattern, replacement in rules]


def scrub(data, rules):
    changed = 0
    for pattern, replacement in rules:
        data, count = pattern.subn(replacement, data)
        changed += count
    return data, changed


def anonymize(source, destination, also=(), keep_screenshots=False):
    """Copies source to destination scrubbed; returns (files copied, replacements, files left out)."""
    rules = patterns(also)
    copied = replaced = left_out = 0
    for path in sorted(Path(source).rglob('*')):
        if not path.is_file():
            continue
        target = Path(destination) / path.relative_to(source)
        suffix = path.suffix.lower()
        if suffix == '.bmp' and not keep_screenshots:
            left_out += 1
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        if suffix in TEXT or path.name == 'info.txt':
            data, count = scrub(path.read_bytes(), rules)
            target.write_bytes(data)
            replaced += count
        else:
            shutil.copyfile(path, target)
        copied += 1
    return copied, replaced, left_out


def compressed_size(path):
    """What the file takes inside a zip (the zip's own deflate), without keeping it."""
    import io
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, 'w', zipfile.ZIP_DEFLATED) as archive:
        archive.write(path, 'x')
    return buffer.tell()


def plan_parts(sizes, limit):
    """Groups of names whose sizes (name -> bytes in the zip) fit in `limit` each, whole files only,
    in name order, a file that is alone over the limit in a group of its own."""
    parts, current, used = [], [], 0
    for name in sorted(sizes):
        cost = sizes[name] + ENTRY_OVERHEAD
        if current and used + cost > limit:
            parts.append(current)
            current, used = [], 0
        current.append(name)
        used += cost
    if current:
        parts.append(current)
    return parts


def make_archives(folder, limit_bytes):
    """Zips `folder` beside itself as <folder>.zip, or as <folder>-partNofM.zip when it does not fit;
    returns the archives' paths. Every archive is under limit_bytes."""
    folder = Path(folder)
    base = str(folder)
    for old in folder.parent.glob(folder.name + '*.zip'):
        old.unlink()
    # A file over the limit even alone is cut into raw pieces first, which are then ordinary files.
    for path in sorted(folder.rglob('*')):
        if path.is_file() and compressed_size(path) + ENTRY_OVERHEAD > limit_bytes:
            piece_size = limit_bytes // 2          # raw pieces compress to no more than this
            with open(path, 'rb') as source:
                for number in range(1, 10**6):
                    chunk = source.read(piece_size)
                    if not chunk:
                        break
                    path.with_name(f'{path.name}.{number:03d}').write_bytes(chunk)
            path.unlink()
    files = {p.relative_to(folder).as_posix(): p for p in folder.rglob('*') if p.is_file()}
    sizes = {name: compressed_size(path) for name, path in files.items()}
    parts = plan_parts(sizes, limit_bytes)
    names = [base + '.zip'] if len(parts) <= 1 else [f'{base}-part{i}of{len(parts)}.zip' for i in range(1, len(parts) + 1)]
    for archive_name, members in zip(names, parts or [[]]):
        with zipfile.ZipFile(archive_name, 'w', zipfile.ZIP_DEFLATED) as archive:
            for name in members:
                archive.write(files[name], name)
        if Path(archive_name).stat().st_size > limit_bytes:
            raise SystemExit(f'{archive_name} is over the limit ({limit_bytes} bytes)')
    return names


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('folder')
    parser.add_argument('--also', action='append', default=[], metavar='WORD',
                        help='another word to blank out (a computer name, your name); may be repeated')
    parser.add_argument('--keep-screenshots', action='store_true')
    parser.add_argument('--limit-mb', type=float, default=LIMIT_MB, metavar='MB',
                        help='the largest file to make, in decimal megabytes; a bigger result is split (default %(default)g)')
    args = parser.parse_args(argv[1:])
    source = Path(args.folder)
    if not source.is_dir():
        raise SystemExit(f'{source}: not a folder')
    destination = source.with_name(source.name + '-shareable')
    if destination.exists():
        shutil.rmtree(destination)
    copied, replaced, left_out = anonymize(source, destination, args.also, args.keep_screenshots)
    archives = make_archives(destination, int(args.limit_mb * 1_000_000))
    print(f'{copied} files copied, {replaced} replacements, {left_out} screenshots left out')
    print(f'Folder: {destination}')
    for archive in archives:
        print(f'Zip:    {archive}')
    if len(archives) > 1:
        print(f'Over {args.limit_mb:g} MB: send all {len(archives)} parts; they unpack into one folder.')
    print('Read info.txt and one log before sending: only what this tool looks for is removed.')


if __name__ == '__main__':
    main(sys.argv)
