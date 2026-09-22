"""Reproduce verified XEX analysis and diagnostic generation in a new directory."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tomllib

from analyse_game import PATTERNS, find_unique_aligned
import analyse_switches
from generate_diagnostic import COUNTRY_CTR, FORMAT_CTR, generate

# These two tables keep their audited whole-function policies, whose hashes
# pin XenonRecomp's plain indirect-branch emission.
AUDITED_CTR_BRANCHES = {branch for branch, _, _ in (*COUNTRY_CTR.values(), *FORMAT_CTR.values())}

ROOT = Path(__file__).resolve().parents[1]


def relative_source_path(source, output, path_api=os.path):
    try:
        return path_api.relpath(source, output)
    except ValueError as error:
        raise ValueError('XEX and output must be on the same drive; copy the XEX '
                         'into this project before preparing the recompilation') from error


def toml_value(value):
    """Serialize one [main] value; integers stay hexadecimal for address review."""
    if isinstance(value, (str, bool)):
        return json.dumps(value)
    if isinstance(value, int):
        return hex(value)
    if isinstance(value, list):
        return '[' + ', '.join(toml_value(item) for item in value) + ']'
    if isinstance(value, dict):
        return '{ ' + ', '.join(f'{key} = {toml_value(item)}' for key, item in value.items()) + ' }'
    raise TypeError(f'unsupported configuration value: {value!r}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--xex', type=Path, default=ROOT / 'private/game/default.xex')
    parser.add_argument('--output', type=Path, default=ROOT / 'out/recomp')
    args = parser.parse_args()
    xex, output = args.xex.resolve(), args.output.resolve()
    try:
        relative_xex = Path(relative_source_path(xex, output)).as_posix()
    except ValueError as error:
        parser.error(str(error))
    expected = json.loads((ROOT / 'config/source.json').read_text())['default_xex']['sha256']
    with xex.open('rb') as source:
        if hashlib.file_digest(source, 'sha256').hexdigest() != expected:
            parser.error('unsupported XEX fingerprint')
    if output.exists():
        parser.error('output must be a new directory; previous analysis is retained')
    subprocess.run([sys.executable, str(ROOT / 'scripts/bootstrap.py'), '--verify-only'], check=True)
    extension = '.exe' if os.name == 'nt' else ''
    host = ROOT / 'out/build/host'
    dump_tool = host / ('sfr_image_dump' + extension)
    recompile = host / 'tools/XenonRecomp/XenonRecomp' / ('XenonRecomp' + extension)
    for tool in (dump_tool, recompile):
        if not tool.is_file():
            parser.error(f'build the host tools first: missing {tool}')
    output.mkdir(parents=True)
    status = {'source_sha256': expected, 'complete': False, 'stage': 'image'}

    def save_status():
        (output / 'status.json').write_text(json.dumps(status, indent=2) + '\n')

    save_status()
    try:
        subprocess.run([str(dump_tool), str(xex), str(output / 'image')], check=True, timeout=60)
        status['stage'] = 'analysis'
        save_status()
        image = (output / 'image/image.bin').read_bytes()
        # XenonAnalyse's fixed switch patterns match none of this game's tables.
        tables, rejected, bctr_sites = analyse_switches.analyse(image)
        tables = [table for table in tables if table['base'] not in AUDITED_CTR_BRANCHES]
        # Functions without .pdata are otherwise sized by XenonRecomp, which
        # stops at the bctr and leaves every case label outside the function.
        table_functions, unplaced = analyse_switches.table_functions(image, tables)
        (output / 'switches.toml').write_text(analyse_switches.to_toml(tables))
        switch_report = analyse_switches.report(tables, rejected, bctr_sites)
        switch_report['table_functions'] = [{**f, 'address': hex(f['address']), 'size': hex(f['size'])}
                                            for f in table_functions]
        switch_report['unplaced_tables'] = unplaced
        (output / 'switches.json').write_text(json.dumps(switch_report, indent=1) + '\n')
        config = tomllib.loads((ROOT / 'config/freeriders.toml').read_text())['main']
        config['functions'] = config.get('functions', []) + [
            {'address': f['address'], 'size': f['size']} for f in table_functions]
        # Verify each configured helper against this decoded executable.
        # The exact source fingerprint fixes .text to this range (also exported
        # in sections.tsv); changes require a new supported-source configuration.
        text_base, text_size = 0x82210000, 0x8BC4EC
        code = image[text_base - 0x82000000:text_base - 0x82000000 + text_size]
        for key, signature in PATTERNS.items():
            if find_unique_aligned(code, bytes.fromhex(signature), text_base) != config[key]:
                raise ValueError(f'helper address differs from configuration: {key}')
        # The pinned upstream concatenates the config directory and each path;
        # absolute paths are therefore invalid even on the same drive.
        config.update(file_path=relative_xex,
                      out_directory_path='ppc', switch_table_file_path='switches.toml')
        (output / 'recomp.toml').write_text('[main]\n' + ''.join(
            f'{key} = {toml_value(value)}\n' for key, value in config.items()))
        (output / 'ppc').mkdir()
        status['stage'] = 'recompile'
        save_status()
        with (output / 'recompile.log').open('w') as log:
            subprocess.run([str(recompile), str(output / 'recomp.toml'),
                            str(ROOT / 'tools/XenonRecomp/XenonUtils/ppc_context.h')],
                           stdout=log, stderr=subprocess.STDOUT, check=True, timeout=300)
        status['stage'] = 'diagnostic-generation'
        save_status()
        report = generate(output / 'ppc', output / 'recompile.log', output / 'diagnostic',
                          output / 'switches.toml')
        status.update(stage='diagnostic-ready', complete=True, playable=False,
                      counts=report['counts'], raw_translation_complete=False,
                      decoded_image_sha256=hashlib.sha256(image).hexdigest())
        save_status()
        print(json.dumps(status, indent=2))
        print(f'Game code generated in {output / "diagnostic"}. Build it with scripts/build_tools.ps1 -Diagnostic '
              f'(Windows) or scripts/build_linux.sh --diagnostic DIR (Linux); see docs/building.md.')
    except BaseException as error:
        status.update(error=str(error))
        save_status()
        raise


if __name__ == '__main__':
    main()
