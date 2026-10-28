#!/usr/bin/env python3
"""Build the trusted romfs service catalog. No runtime-writable privileges."""
import json
import struct
import sys
from pathlib import Path


def build(entries):
    if not isinstance(entries, list) or not 1 <= len(entries) <= 8:
        raise ValueError('service count must be 1..8')
    names = [e['name'] for e in entries]
    if len(set(names)) != len(names):
        raise ValueError('duplicate service name')
    caps = {'privileged': 1, 'flash': 2, 'network': 4, 'audio': 8}
    result = []
    for e in entries:
        name, path = e['name'], e['path']
        if (not name.isascii() or not name.replace('-', '').replace('_', '').isalnum()
                or len(name) >= 16 or not path.isascii() or len(path) >= 48
                or not path.startswith('bin/') or '/' in path[4:] or not path[4:].replace('_', '').replace('-', '').isalnum()):
            raise ValueError('invalid service name or romfs path')
        privileges = 0
        for cap in e.get('privileges', []):
            privileges |= caps[cap]
        required = e.get('requires', [])
        if any(r != 'network' for r in required):
            raise ValueError('unknown hardware requirement')
        policy = int('network' in required) | (2 if e.get('required', False) else 0)
        if privileges & 4 and not policy & 1:
            raise ValueError('network privileges require a network dependency')
        after = sum(1 << names.index(n) for n in set(e.get('after', [])))
        result.append(struct.pack('<16s48sIII', name.encode(), path.encode(), privileges, policy, after))
    done = 0
    for _ in entries:
        for i, e in enumerate(entries):
            deps = struct.unpack_from('<I', result[i], 72)[0]
            if deps & done == deps:
                done |= 1 << i
    if done != (1 << len(entries)) - 1:
        raise ValueError('cyclic service dependencies')
    return struct.pack('<III', 0x475a5356, 1, len(entries)) + b''.join(result)


if __name__ == '__main__':
    try:
        data = build(json.loads(Path(sys.argv[1]).read_text()))
        Path(sys.argv[2]).write_bytes(data)
    except (ValueError, KeyError, TypeError) as error:
        sys.exit(f'invalid service catalog: {error}')
