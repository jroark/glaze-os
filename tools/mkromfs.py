#!/usr/bin/env python3
"""Build the Glaze romfs archive (see kernel/romfs.c for the format).

usage: mkromfs.py OUT NAME=PATH [NAME=PATH ...]
"""

import struct
import sys

ENTRY = struct.Struct("<48sII")
ALIGN = 4096


def main() -> int:
    out = sys.argv[1]
    files = []
    for arg in sys.argv[2:]:
        name, path = arg.split("=", 1)
        if len(name.encode()) >= 48:
            raise SystemExit(f"romfs name too long: {name}")
        files.append((name, open(path, "rb").read()))
    files.sort()

    header_len = 8 + ENTRY.size * len(files)
    offset = (header_len + ALIGN - 1) // ALIGN * ALIGN
    table = bytearray(b"GZRF" + struct.pack("<I", len(files)))
    blobs = bytearray()
    for name, data in files:
        table += ENTRY.pack(name.encode(), offset + len(blobs), len(data))
        blobs += data
        blobs += b"\0" * (-len(blobs) % ALIGN)
    image = table + b"\0" * (offset - len(table)) + blobs
    open(out, "wb").write(image)
    print(f"[mkromfs] {out}: {len(files)} files, {len(image) // 1024} KiB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
