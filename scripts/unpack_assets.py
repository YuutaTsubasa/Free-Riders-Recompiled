#!/usr/bin/env python3
"""Join the game's decompressed output back into whole files.

    python scripts/unpack_assets.py <dump directory> <game directory> <output directory> [name ...]

The dump comes from a run with SFR_DUMP_DECOMPRESSED=<dump directory>: one
<n>.bin per call of the game's LZX decoder, and index.txt with
"<n> <size> <compressed size> <context> <first 64 compressed bytes in hex>"
for a call and "R <context>" where the game starts a decoder. A context goes
on from one file to the next, and the calls for one file pass the same
compressed data and return the file part by part. The data is found in the
game's compressed files (they start 0F F5 12 ED), and the first load of each
file is written out. Only files the run loaded come out; names limit the
output to those files.

Nothing here contains game data: it works on the player's own files.
"""
import sys
from pathlib import Path

MAGIC = bytes.fromhex('0ff512ed')


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    dump, game, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    wanted = {name.lower() for name in sys.argv[4:]}
    files = {}
    for path in sorted(game.rglob('*')):
        if not path.is_file() or path.stat().st_size < 20:
            continue
        if wanted and path.name.lower() not in wanted:
            continue
        with path.open('rb') as f:
            if f.read(4) != MAGIC:
                continue
        files[path] = path.read_bytes()

    # Streams in the order they started: [file or None, [(n, size), ...]].
    streams, open_by_context, located = [], {}, {}
    for line in (dump / 'index.txt').read_text().splitlines():
        fields = line.split()
        if fields[0] == 'R':
            stream = [None, []]
            streams.append(stream)
            open_by_context[fields[1]] = stream
            continue
        n, size, _, context, head = fields
        path = located.get(head)
        if path is None:
            prefix = bytes.fromhex(head)
            path = located[head] = next((p for p, data in files.items() if data.find(prefix) >= 0), False)
        stream = open_by_context.get(context)
        # A context goes on to the next file without a new start.
        if stream is None or (stream[0] is not None and stream[0] != path):
            stream = [None, []]
            streams.append(stream)
            open_by_context[context] = stream
        stream[0] = path
        stream[1].append((int(n), int(size)))

    out.mkdir(parents=True, exist_ok=True)
    written = set()
    for path, parts in streams:
        if not path or path in written:
            continue
        written.add(path)
        joined = bytearray()
        for n, size in parts:
            block = (dump / f'{n}.bin').read_bytes()
            if len(block) != size:
                raise SystemExit(f'{n}.bin: {len(block)} bytes, index says {size}')
            joined += block
        name = path.relative_to(game)
        target = out / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(joined)
        print(f'{name}: {len(parts)} parts, {len(joined)} bytes')
    return 0


if __name__ == '__main__':
    sys.exit(main())
