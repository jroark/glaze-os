#!/usr/bin/env python3
"""Build the Glaze romfs archive (see kernel/romfs.c for the format), or an
app package, which is the same container with the magic "GZPK" (see
lib/include/glaze/pkg.h).

usage: mkromfs.py [--package | --lz4] OUT NAME=PATH [NAME=PATH ...]

--lz4 stores every romfs file as an LZ4 page container (see tools/romfs.py),
marked by a ".lz4" entry. The table keeps describing the physical layout, so
SPLs that check an OTA's romfs table accept it; kernel/romfs.c inflates pages
on demand.
"""

import struct
import sys
from pathlib import Path

ENTRY = struct.Struct("<48sII")


def main() -> int:
    args = sys.argv[1:]
    package = args[:1] == ["--package"]
    lz4 = args[:1] == ["--lz4"]
    if package or lz4:
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
    logical = sum((len(d) + 4095) // 4096 * 4096 for _, d in files)
    if lz4:
        # Only here: the SDK ships this script without tools/romfs.py.
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import romfs
        files = [(name, romfs.lz4_container(data)) for name, data in files]
        files.append((romfs.LZ4_MARKER, romfs.LZ4_MARKER_DATA))
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
    note = f" (LZ4; {logical // 1024} KiB uncompressed)" if lz4 else ""
    print(f"[{kind}] {out}: {len(files)} files, {len(image) // 1024} KiB{note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
