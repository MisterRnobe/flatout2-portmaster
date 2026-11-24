#!/usr/bin/env python3
"""Prepare an isolated, repeatable race-test profile; never write source saves."""
import argparse
import ctypes
import os
import sys
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ('FlatOut2.exe', 'd3dx9_30.dll', 'data.bfs', 'zoom_platform.bfs')

def copy_asset(source: Path, target: Path):
    # The guest confines resolved paths to its game root: external symlinks fail.
    # APFS clones are independent files with copy-on-write storage, not hardlinks.
    if sys.platform == 'darwin':
        libc = ctypes.CDLL(None, use_errno=True)
        clone = libc.clonefile
        clone.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int)
        clone.restype = ctypes.c_int
        if clone(os.fsencode(source), os.fsencode(target), 0) == 0:
            shutil.copystat(source, target)
            return
    # Filesystems without cloning still get an independent, confined copy.
    shutil.copy2(source, target)


def prepare(source: Path, session: Path, seed: Path) -> Path:
    source, session, seed = source.resolve(), session.resolve(), seed.resolve()
    for name in (*ASSETS, 'filesystem'):
        if not (source / name).is_file():
            raise ValueError(f'Missing game asset: {source / name}')
    if not (source / 'Savegame').is_dir():
        raise ValueError('The automatic race needs an existing game profile')
    if not seed.exists():
        seed.parent.mkdir(parents=True, exist_ok=True)
        temporary = seed.with_name(seed.name + '.preparing')
        if temporary.exists():
            raise ValueError(f'Incomplete scenario seed exists: {temporary}')
        shutil.copytree(source / 'Savegame', temporary)
        temporary.rename(seed)
    target = session / 'gamedata'
    if target.exists():
        raise ValueError(f'Scenario directory already exists: {target}')
    target.mkdir(parents=True)
    for name in ASSETS:
        copy_asset(source / name, target / name)
    shutil.copy2(source / 'filesystem', target / 'filesystem')
    shutil.copytree(seed, target / 'Savegame')
    return target

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--session', type=Path, required=True)
    parser.add_argument('--seed', type=Path, default=ROOT / 'work/metal/scenarios/auto-race/seed')
    args = parser.parse_args()
    try:
        print(prepare(args.source, args.session, args.seed))
    except (OSError, ValueError) as error:
        parser.exit(1, f'Scenario preparation failed: {error}\n')
