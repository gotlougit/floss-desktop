#!/usr/bin/env python3
"""Copy only distributable Nix inputs, excluding reference checkouts."""
import argparse
import shutil
from pathlib import Path
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('destination', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
destination = args.destination.resolve()
if destination == root or root.is_relative_to(destination):
    parser.error('destination must not contain the workspace')
destination.mkdir(parents=True, exist_ok=True)
for name in ('flake.nix', 'flake.lock'):
    shutil.copy2(root / name, destination / name)
for name in ('nix', 'patches'):
    shutil.copytree(root / name, destination / name, dirs_exist_ok=True)
print(destination)
