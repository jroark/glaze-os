#!/usr/bin/env python3
"""Unpack packages as if installed, for a test image's factory volume.

  tests/hwpkg_fs.py DIR PKG.gzp [PKG.gzp ...]

writes each package's entries under DIR/<id>.app/ and prints the FS_EXTRA
arguments (mk/fs.mk) that put them at /Applications/<id>.app on the flash:

  make FS_EXTRA="$(tests/hwpkg_fs.py out/hwtest out/pkg/hw-pccards.gzp)"
"""
import struct
import sys
from pathlib import Path

ENTRY = struct.Struct("<48sII")


def main():
    out = Path(sys.argv[1])
    args = []
    for pkg in sys.argv[2:]:
        data = Path(pkg).read_bytes()
        assert data[:4] == b"GZPK", pkg
        (count,) = struct.unpack_from("<I", data, 4)
        entries = {}
        for i in range(count):
            name, off, size = ENTRY.unpack_from(data, 8 + i * ENTRY.size)
            entries[name.rstrip(b"\0").decode()] = data[off:off + size]
        info = dict(line.split("=", 1) for line in entries["Info"].decode().splitlines() if "=" in line)
        bundle = out / f"{info['id']}.app"
        for name, blob in entries.items():
            path = bundle / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(blob)
            args.append(f"/Applications/{info['id']}.app/{name}={path}")
    print(" ".join(args))


if __name__ == "__main__":
    main()
