"""Find conservative PPC helper candidates in the verified decoded image."""
import argparse
import csv
import json
from pathlib import Path


def find_unique_aligned(data: bytes, pattern: bytes, base: int) -> int:
    if not pattern:
        raise ValueError('empty signature')
    matches = [base + i for i in range(0, len(data) - len(pattern) + 1, 4)
               if data.startswith(pattern, i)]
    if not matches:
        raise ValueError(f'missing signature {pattern.hex()}')
    if len(matches) != 1:
        raise ValueError(f'ambiguous signature {pattern.hex()}: {len(matches)} matches')
    return matches[0]


PATTERNS = {
    'restgprlr_14_address': 'e9c1ff68e9e1ff70',
    'savegprlr_14_address': 'f9c1ff68f9e1ff70',
    'restfpr_14_address': 'c9ccff70c9ecff78',
    'savefpr_14_address': 'd9ccff70d9ecff78',
    'restvmx_14_address': '3960fee07dcb60ce',
    'savevmx_14_address': '3960fee07dcb61ce',
    'restvmx_64_address': '3960fc00100b60cb',
    'savevmx_64_address': '3960fc00100b61cb',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dump', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if not (args.dump / 'complete.txt').is_file():
        parser.error('image dump has no completion marker')
    metadata = dict(line.split('\t') for line in (args.dump / 'image.tsv').read_text().splitlines())
    base = int(metadata['base'])
    image = (args.dump / 'image.bin').read_bytes()
    if len(image) != int(metadata['size']):
        parser.error('image size mismatch')
    with (args.dump / 'sections.tsv').open() as stream:
        sections = list(csv.DictReader(stream, delimiter='\t'))
    code = [s for s in sections if int(s['flags']) & 2]
    report = {'metadata': metadata, 'helper_candidates': {}, 'unresolved': {}}
    for name, signature in PATTERNS.items():
        found = []
        for section in code:
            address, size = int(section['address']), int(section['size'])
            offset = address - base
            if offset < 0 or offset + size > len(image):
                parser.error('section outside image')
            try:
                found.append(find_unique_aligned(image[offset:offset+size], bytes.fromhex(signature), address))
            except ValueError as error:
                if 'ambiguous' in str(error):
                    report['unresolved'][name] = str(error)
        if len(found) == 1 and name not in report['unresolved']:
            report['helper_candidates'][name] = f'0x{found[0]:08X}'
        else:
            report['unresolved'].setdefault(name, f'expected one match, found {len(found)}')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return 1 if report['unresolved'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
