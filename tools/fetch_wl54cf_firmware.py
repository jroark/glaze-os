#!/usr/bin/env python3
"""Fetch the unmodified, redistributable Linux CF8385 firmware. See docs/WL54CF.md."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
from urllib.request import urlopen


def verified(data, entry):
    return len(data) == entry['size_bytes'] and hashlib.sha256(data).hexdigest() == entry['sha256']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('firmware/wl54cf'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    manifest = json.loads((root / 'firmware/wl54cf-linux.json').read_text())
    base = manifest['repository'] + '/+/' + manifest['revision'] + '/'
    downloads = {}
    for name, entry in manifest['files'].items():
        with urlopen(base + name + '?format=TEXT', timeout=30) as response:
            data = base64.b64decode(response.read(), validate=True)
        if not verified(data, entry):
            raise SystemExit(f'Unexpected size or SHA-256 for {name}; no files written')
        downloads[Path(name).name] = data
    if downloads['LICENCE.Marvell'] != (root / 'firmware/LICENCE.Marvell').read_bytes():
        raise SystemExit('Downloaded license differs from the included notice; no files written')
    args.output.mkdir(parents=True, exist_ok=True)
    for name, data in downloads.items():
        (args.output / name).write_bytes(data)
        print(f'{name}: {len(data)} bytes; SHA-256 verified')


if __name__ == '__main__':
    main()
