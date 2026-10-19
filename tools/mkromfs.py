#!/usr/bin/env python3
"""Build the Glaze romfs archive (see kernel/romfs.c for the format), or an
app package, which is the same container with the magic "GZPK" (see
lib/include/glaze/pkg.h).

usage: mkromfs.py [--package] OUT NAME=PATH [NAME=PATH ...]
"""

import struct
import sys

ENTRY = struct.Struct("<48sII")


def main() -> int:
    args = sys.argv[1:]
    package = args[:1] == ["--package"]
    if package:
        args = args[1:]
    # romfs files are page-aligned so the kernel can map them; a package is
    # only ever read, so word alignment will do.
    magic, align = (b"GZPK", 4) if package else (b"GZRF", 4096)
    out = args[0]
    files = []
    for arg in args[1:]:
        name, path = arg.split("=", 1)
        if len(name.encode()) >= 48:
            raise SystemExit(f"romfs name too long: {name}")
        files.append((name, open(path, "rb").read()))
    files.sort()

    header_len = 8 + ENTRY.size * len(files)
    offset = (header_len + align - 1) // align * align
    table = bytearray(magic + struct.pack("<I", len(files)))
    blobs = bytearray()
    for name, data in files:
        table += ENTRY.pack(name.encode(), offset + len(blobs), len(data))
        blobs += data
        blobs += b"\0" * (-len(blobs) % align)
    image = table + b"\0" * (offset - len(table)) + blobs
    open(out, "wb").write(image)
    kind = "mkpkg" if package else "mkromfs"
    print(f"[{kind}] {out}: {len(files)} files, {len(image) // 1024} KiB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
