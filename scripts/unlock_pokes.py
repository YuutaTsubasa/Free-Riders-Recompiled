#!/usr/bin/env python3
"""SFR_POKE words that unlock things in the signed-in player's record (testing).

    python scripts/unlock_pokes.py <record.bin> <record address> <present> [set ...]

<record.bin> is what SFR_ACHIEVEMENT_TRACE=<file> wrote in an earlier run of
the same route, and the address is the record= of its ACHIEVEMENT_PROGRESS
line (0x70137d70 on Windows and Linux so far). The output is the value for
SFR_POKE: only the words whose bytes change, so the record's other bytes keep
what the game had. Sets (docs/saves.md, src/achievement_progress.cpp):

    emeralds   gear 38 owned: the Chaos Emeralds (Super Sonic) on Sonic's gear ring
    gears      every gear owned          parts      every gear part owned
    tutorials  every tutorial topic      licenses   every course of every mode
    all        all of the above plus the 1st-place, X-rank trick and start-dash sets
"""
import sys

SETS = {
    'emeralds': [(0x09D8 + 38, 1)],
    'gears': [(0x09D8, 46)],
    'parts': [(0x0A62, 21)],
    'tutorials': [(0x09A8, 27)],
    'licenses': [(0x1958, 80)],
}
SETS['all'] = (SETS['gears'] + SETS['parts'] + SETS['tutorials'] + SETS['licenses'] +
               [(0x19A8, 19), (0x19BB, 46), (0x1A0C, 16), (0x1A24, 16)])


def pokes(record: bytes, address: int, present: int, names) -> str:
    new = bytearray(record)
    for name in names:
        for offset, count in SETS[name]:
            new[offset:offset + count] = b'\x01' * count
    words = []
    for at in range(0, len(record) - 3, 4):
        if new[at:at + 4] != record[at:at + 4]:
            words.append('%x=%d@%d' % (address + at, int.from_bytes(new[at:at + 4], 'big'), present))
    return ','.join(words)


if __name__ == '__main__':
    if len(sys.argv) < 4 or any(name not in SETS for name in sys.argv[4:]):
        sys.exit(__doc__)
    with open(sys.argv[1], 'rb') as f:
        record = f.read()
    print(pokes(record, int(sys.argv[2], 16), int(sys.argv[3]), sys.argv[4:] or ['emeralds']))
