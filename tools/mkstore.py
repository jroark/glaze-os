#!/usr/bin/env python3
"""Generate a portable, validated Glaze App Store from built .gzp files."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

FIELDS = {'id':24, 'name':31, 'version':15, 'developer':47, 'summary':79, 'description':511}

def package(path):
    data = path.read_bytes()
    if not 8 <= len(data) <= 2 * 1024 * 1024 or data[:4] != b'GZPK':
        raise ValueError(f'{path}: invalid package')
    count, = struct.unpack_from('<I', data, 4)
    if not 1 <= count <= 64 or 8 + count * 56 > len(data):
        raise ValueError(f'{path}: invalid table')
    entries = {}
    for i in range(count):
        raw, off, size = struct.unpack_from('<48sII', data, 8 + i * 56)
        if b'\0' not in raw:
            raise ValueError('unterminated entry')
        name = raw.split(b'\0')[0].decode('ascii')
        if (not re.fullmatch(r'[A-Za-z0-9_./-]+', name) or any(x in ('', '.', '..') for x in name.split('/'))
            or name.split('/')[0] in ('data', '.pkg-ready') or name in entries
            or off < 8 + count * 56 or off + size > len(data)):
            raise ValueError(f'{path}: invalid entry {name}')
        entries[name] = data[off:off+size]
    info = dict(line.split('=', 1) for line in entries['Info'].decode('ascii').splitlines() if '=' in line)
    if '/' in info.get('exec', 'app') or info.get('exec', 'app') not in entries:
        raise ValueError('missing executable')
    elf = entries[info.get('exec', 'app')]
    if elf[:7] != b'\x7fELF\x01\x01\x01' or len(elf) < 52 or struct.unpack_from('<H', elf, 18)[0] != 8:
        raise ValueError('expected little-endian MIPS ELF')
    if struct.unpack_from('<H', elf, 16)[0] != 3:
        raise ValueError('expected a dynamically linked PIE')
    phoff, = struct.unpack_from('<I', elf, 28)
    phsize, phcount = struct.unpack_from('<HH', elf, 42)
    if phsize != 32 or phoff + phsize * phcount > len(elf):
        raise ValueError('invalid ELF program headers')
    interpreters = []
    for i in range(phcount):
        kind, off, _, _, size = struct.unpack_from('<IIIII', elf, phoff+i*phsize)
        if kind == 3:
            interpreters.append(elf[off:off+size])
    if interpreters != [b'/lib/ld-glaze.so.1\0']:
        raise ValueError('expected the Glaze dynamic linker')
    icon = entries['res/icon.gzi']
    if len(icon) < 12 or icon[:4] != b'GZI1':
        raise ValueError('invalid icon')
    w,h,flags,_ = struct.unpack_from('<HHHH', icon, 4)
    if (w,h) != (44,44) or flags not in (0,1) or len(icon) != 12+w*h*(3 if flags else 2):
        raise ValueError('expected 44x44 GZI icon')
    return data, info, icon

def immutable(path, data):
    if path.exists() and path.read_bytes() != data:
        raise ValueError(f'{path}: immutable asset changed; bump the package version')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)

def build(metadata, packages, output):
    apps = json.loads(metadata.read_text())
    if len(apps) > 32:
        raise ValueError('too many apps')
    seen = set()
    records = ['GZSTORE1\n']
    # Validate every input before writing anything.
    prepared = []
    for item in apps:
        ident = item['id']
        if ident in seen or not re.fullmatch(r'[a-z0-9][a-z0-9.-]{0,23}', ident) or '..' in ident:
            raise ValueError('invalid or duplicate id')
        seen.add(ident)
        data, info, icon = package(packages / f'{ident}.gzp')
        if info['id'] != ident:
            raise ValueError('package identity mismatch')
        app = {key: info[key] if key in ('id','name','version') else item[key] for key in FIELDS}
        for key, limit in FIELDS.items():
            value = app[key]
            if not isinstance(value, str) or not 0 < len(value) <= limit or any(not 32 <= ord(c) <= 126 for c in value):
                raise ValueError(f'invalid {key}')
        if not re.fullmatch(r'\d{1,6}(\.\d{1,6})*', app['version']):
            raise ValueError('invalid version')
        base = f'apps/{ident}/{app["version"]}'
        app.update(package=f'{base}/app.gzp', icon=f'{base}/icon.gzi', size=len(data),
                   sha256=hashlib.sha256(data).hexdigest(), icon_sha256=hashlib.sha256(icon).hexdigest())
        for path, content in ((output/app['package'], data), (output/app['icon'], icon)):
            if path.exists() and path.read_bytes() != content:
                raise ValueError(f'{path}: immutable asset changed; bump the version')
        prepared.append((app, data, icon))
        records.append('[app]\n' + ''.join(f'{k}={v}\n' for k,v in app.items()) + '\n')
    catalog = ''.join(records).encode('ascii')
    if len(catalog) > 65536:
        raise ValueError('catalog too large')
    for app, data, icon in prepared:
        immutable(output/app['package'], data)
        immutable(output/app['icon'], icon)
    output.mkdir(parents=True, exist_ok=True)
    (output/'catalog.txt.tmp').write_bytes(catalog)
    (output/'catalog.txt.tmp').replace(output/'catalog.txt')
    (output/'_headers').write_text('/catalog.txt\n  Cache-Control: no-cache\n/apps/*\n  Cache-Control: public, max-age=31536000, immutable\n  Content-Encoding: identity\n')
    print(f'[store] {len(prepared)} apps -> {output}')

if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--metadata', type=Path, default=Path('store/apps.json'))
    ap.add_argument('--packages', type=Path, default=Path('out/pkg'))
    ap.add_argument('--output', type=Path, default=Path('out/store'))
    args = ap.parse_args()
    try:
        build(args.metadata, args.packages, args.output)
    except (ValueError, KeyError, UnicodeError, struct.error) as e:
        ap.exit(1, f'mkstore: {e}\n')
