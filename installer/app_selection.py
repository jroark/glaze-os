"""Offline app selection for a device-specific NAND image; no device access."""
import hashlib
import json
from pathlib import Path, PurePosixPath
import struct

ENTRY = struct.Struct('<48sII')
ROMFS_START, ROMFS_END = 0x40000, 0x500000


def entries(blob, magic):
    if len(blob) < 8 or blob[:4] != magic:
        raise ValueError('Invalid app archive signature')
    count = struct.unpack_from('<I', blob, 4)[0]
    end = 8 + count * ENTRY.size
    if count > 256 or end > len(blob):
        raise ValueError('Invalid app archive table')
    result = {}
    for i in range(count):
        name, offset, size = ENTRY.unpack_from(blob, 8 + i * ENTRY.size)
        name = name.split(b'\0', 1)[0].decode('utf-8')
        parts = PurePosixPath(name)
        if (not name or '\\' in name or ':' in name or parts.is_absolute() or
                any(p in ('', '.', '..') for p in name.split('/')) or name in result or
                offset < end or offset + size > len(blob)):
            raise ValueError('Invalid app archive entry: ' + name)
        result[name] = blob[offset:offset + size]
    return result


def archive(files):
    offset = (8 + len(files) * ENTRY.size + 4095) & ~4095
    table = bytearray(b'GZRF' + struct.pack('<I', len(files)))
    blobs = bytearray()
    for name, data in sorted(files.items()):
        table += ENTRY.pack(name.encode(), offset + len(blobs), len(data))
        blobs += data
        blobs += b'\0' * (-len(blobs) % 4096)
    return table + bytes(offset - len(table)) + blobs


def load_catalog(resources):
    resources = Path(resources)
    path = resources / 'apps.json'
    expected = json.loads((resources / 'manifest.json').read_text())['files'].get('apps.json')
    if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
        raise ValueError('App catalog failed verification')
    return json.loads(path.read_text())


def select(catalog, requested=None):
    known = {app['id'] for app in catalog}
    chosen = set(requested if requested is not None else [a['id'] for a in catalog if a['default']])
    if chosen - known:
        raise ValueError('Unknown app selection: ' + ', '.join(sorted(chosen - known)))
    chosen.update(a['id'] for a in catalog if a.get('required'))
    return sorted(chosen)


def customize_logical(logical, catalog, chosen):
    image = bytearray(logical)
    if len(image) != 16 * 1024 * 1024:
        raise ValueError('Invalid NAND image size')
    size = struct.unpack_from('<I', image, 4 * 16 + 12)[0] * 512
    if size > ROMFS_END - ROMFS_START:
        raise ValueError('Invalid ROMFS partition size')
    files = entries(image[ROMFS_START:ROMFS_START + size], b'GZRF')
    for app in catalog:
        if app['kind'] == 'builtin':
            name = 'bin/' + app['program']
            if name not in files:
                raise ValueError('Bundled OS does not contain ' + app['name'])
            if app['id'] not in chosen:
                del files[name]
    blob = archive(files)
    if len(blob) > ROMFS_END - ROMFS_START:
        raise ValueError('Selected apps exceed the ROMFS slot')
    image[ROMFS_START:ROMFS_END] = b'\xff' * (ROMFS_END - ROMFS_START)
    image[ROMFS_START:ROMFS_START + len(blob)] = blob
    struct.pack_into('<I', image, 4 * 16 + 12, (len(blob) + 511) // 512)
    return bytes(image)


def install_packages(resources, catalog, chosen, userdata):
    resources, userdata = Path(resources), Path(userdata)
    for app in catalog:
        if app['kind'] != 'package' or app['id'] not in chosen:
            continue
        package = resources / 'packages' / (app['program'] + '.gzp')
        files = entries(package.read_bytes(), b'GZPK')
        info = dict(line.split('=', 1) for line in files.get('Info', b'').decode().splitlines() if '=' in line)
        if info.get('id') != app['program'] or info.get('exec', 'app') not in files:
            raise ValueError('Package metadata does not match ' + app['name'])
        bundle = userdata / 'Applications' / (app['program'] + '.app')
        bundle.mkdir(parents=True, exist_ok=False)
        for name, data in files.items():
            if name == 'data' or name.startswith('data/') or any(p.startswith('.pkg-') for p in name.split('/')):
                raise ValueError('Package contains reserved app data')
            target = bundle / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        (bundle / '.install-id').write_text('1')
