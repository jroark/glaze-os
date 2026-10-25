#!/usr/bin/env python3
"""Extract local firmware from the supplied AmbiCom WinCE 4.1 CAB or DLL.

No driver/firmware files are redistributed. Exact hashes deliberately reject
other packages: offsets below were verified against this AMWL54G.dll only.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

DLL_HASH = 'e38d1aa0700da233bbca0d191d475b4faba3edd9b3f6887420cb97c5725155fc'
IMAGES = (
    ('cf8385_helper.bin', 0x14AC0, 2136,
     'c29b4c977fc376bc3a486c0ebd9b5af30e582022a5831801f61dbfc621c9af35'),
    ('cf8385.bin', 86808, 97128,
     'f9bf67179e17b0d88d5788f196a71d28c8db3afee3aae298dfffbfb4dc757b86'),
)


def extract(data, destination):
    if hashlib.sha256(data).hexdigest() != DLL_HASH:
        raise ValueError('unsupported AMWL54G.dll: SHA-256 does not match supplied package')
    images = []
    for name, offset, size, digest in IMAGES:
        image = data[offset:offset + size]
        if hashlib.sha256(image).hexdigest() != digest:
            raise ValueError(f'{name}: firmware hash mismatch')
        images.append((name, image, digest))
    destination.mkdir(parents=True, exist_ok=True)
    for name, image, digest in images:
        (destination / name).write_bytes(image)
        print(f'{name}: {len(image)} bytes, SHA-256 {digest}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('package', type=Path)
    parser.add_argument('--output', type=Path, default=Path('firmware/wl54cf'))
    args = parser.parse_args()
    data = args.package.read_bytes()
    if data.startswith(b'MSCF'):
        with tempfile.TemporaryDirectory(prefix='wl54cf-') as directory:
            subprocess.run(['cabextract', '-q', '-d', directory, str(args.package)], check=True)
            candidates = [p.read_bytes() for p in Path(directory).iterdir() if p.is_file()]
            matches = [b for b in candidates if hashlib.sha256(b).hexdigest() == DLL_HASH]
            if len(matches) != 1:
                parser.error('CAB does not contain the supported ARM AMWL54G.dll')
            data = matches[0]
    try:
        extract(data, args.output)
    except ValueError as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
