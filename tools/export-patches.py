#!/usr/bin/env python3
"""Export portable changes against pinned upstream revisions, without staging."""
import argparse
import base64
import hashlib
import json
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--project', choices=['bluetooth', 'pipewire', 'bluedevil', 'wireplumber'])
parser.add_argument('--no-manifest', action='store_true', help='Leave manifest updates to the final combined export')
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
manifest = root / 'patches/sources.json'
sources = json.loads(manifest.read_text())
for name, entry in sources.items():
    if args.project and args.project != name:
        continue
    repo = root / ('Bluetooth' if name == 'bluetooth' else name)
    def git(*arguments):
        return subprocess.check_output(['git', *arguments], cwd=repo)
    if git('rev-parse', 'HEAD').decode().strip() != entry['rev']:
        raise SystemExit(f'{name}: base revision differs from manifest')
    extra_patches = entry.get('extraPatches', [])
    extra_files = {filename for patch in extra_patches for filename in patch['files']}
    def belongs_to_patch(filename, patch):
        return any(filename == path or (path.endswith('/') and filename.startswith(path))
                   for path in patch['files'])
    payload = git('diff', '--binary', '--no-ext-diff', '--no-textconv', 'HEAD', '--', '.',
                  *(f':(exclude){filename}' for filename in sorted(extra_files)))
    extra_payloads = [git('diff', '--binary', '--no-ext-diff', '--no-textconv',
                          'HEAD', '--', *patch['files']) for patch in extra_patches]
    for filename in git('ls-files', '--others', '--exclude-standard', '-z').split(b'\0'):
        if not filename:
            continue
        diff = subprocess.run(['git', 'diff', '--binary', '--no-ext-diff', '--no-textconv',
                               '--no-index', '--', '/dev/null', filename.decode()],
                              cwd=repo, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if diff.returncode not in (0, 1):
            raise SystemExit(diff.stderr.decode())
        matches = [index for index, patch in enumerate(extra_patches)
                   if belongs_to_patch(filename.decode(), patch)]
        if matches:
            if len(matches) != 1:
                raise SystemExit(f'{name}: overlapping patch file sets for {filename.decode()}')
            extra_payloads[matches[0]] += diff.stdout
        else:
            payload += diff.stdout
    for patch, patch_payload in [(entry, payload), *zip(extra_patches, extra_payloads)]:
        path = root / 'patches' / patch['patch']
        path.write_bytes(patch_payload)
        patch['patchBytes'] = len(patch_payload)
        patch['patchFileHash'] = 'sha256-' + base64.b64encode(hashlib.sha256(patch_payload).digest()).decode()
        print(f'{name}/{patch["patch"]}: {len(patch_payload)} bytes, {patch["patchFileHash"]}')
if not args.no_manifest:
    manifest.write_text(json.dumps(sources, indent=2) + '\n')
