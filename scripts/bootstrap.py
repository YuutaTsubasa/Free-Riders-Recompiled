"""Fetch exact public tool revisions; never resets an existing checkout.

A dependency may list patches (files under patches/) that bootstrap applies
to a fresh checkout. An existing checkout must carry exactly those changes.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def git(*args, cwd=None):
    result = subprocess.run(['git', *args], cwd=cwd, check=True, text=True, capture_output=True)
    return result.stdout.strip()


def patched_diff(path, patches):
    """The combined diff the listed patches make, applied to a scratch index."""
    if not patches:
        return ''
    index = path / '.git' / 'sfr-patch-index' if (path / '.git').is_dir() else None
    environment = dict(os.environ)
    if index:
        environment['GIT_INDEX_FILE'] = str(index)
    try:
        subprocess.run(['git', 'read-tree', 'HEAD'], cwd=path, check=True, env=environment, capture_output=True)
        for patch in patches:
            subprocess.run(['git', 'apply', '--cached', str(ROOT / patch)], cwd=path, check=True,
                           env=environment, capture_output=True)
        return subprocess.run(['git', 'diff', '--cached', 'HEAD'], cwd=path, check=True, env=environment,
                              capture_output=True, text=True).stdout
    finally:
        if index and index.exists():
            index.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify-only', action='store_true')
    args = parser.parse_args()
    lock = json.loads((ROOT / 'config/dependencies.lock.json').read_text(encoding='utf-8-sig'))
    for dependency in lock['dependencies']:
        path = ROOT / dependency['path']
        if not path.resolve().is_relative_to(ROOT):
            raise ValueError('dependency path is outside project')
        if not path.exists():
            if args.verify_only:
                raise ValueError(f'missing dependency: {path}')
            path.parent.mkdir(parents=True, exist_ok=True)
            git('clone', '--no-checkout', dependency['url'], str(path))
            git('checkout', '--detach', dependency['commit'], cwd=path)
            git('submodule', 'update', '--init', '--recursive', cwd=path)
            for patch in dependency.get('patches', []):
                git('apply', str(ROOT / patch), cwd=path)
        actual = git('rev-parse', 'HEAD', cwd=path)
        if actual != dependency['commit']:
            raise ValueError(f'{path}: revision differs from lock; existing checkout was not changed')
        patches = dependency.get('patches', [])
        if git('status', '--porcelain', '--untracked-files=no', '--ignore-submodules=all', cwd=path):
            changes = subprocess.run(['git', 'diff', 'HEAD'], cwd=path, check=True, capture_output=True,
                                     text=True).stdout
            if not patches or changes != patched_diff(path, patches):
                raise ValueError(f'{path}: tracked upstream files are modified beyond the listed patches')
        elif patches:
            if args.verify_only:
                raise ValueError(f'{path}: listed patches are not applied')
            for patch in patches:
                git('apply', str(ROOT / patch), cwd=path)
        for submodule in dependency['submodules']:
            subpath = path / submodule['path']
            # A submodule added to the lock after the checkout was made.
            if not (subpath / '.git').exists():
                if args.verify_only:
                    raise ValueError(f'missing submodule: {subpath}')
                git('submodule', 'update', '--init', '--', submodule['path'], cwd=path)
            if git('rev-parse', 'HEAD', cwd=subpath) != submodule['commit']:
                raise ValueError(f'{subpath}: revision differs from lock')
            if git('status', '--porcelain', '--untracked-files=no', '--ignore-submodules=all', cwd=subpath):
                raise ValueError(f'{subpath}: tracked files are modified')
        print(f"Verified {dependency['name']} {actual}")
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(f'bootstrap: {error}')
