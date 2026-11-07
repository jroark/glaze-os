"""Glaze romfs archives (kernel/romfs.c): reading, and the LZ4 page
containers of compressed archives (tools/mkromfs.py --lz4).

A compressed archive has a ".lz4" entry. Each other file is then stored as
  "GZL4", u32 size, u32 pages, pages x (u32 offset, u32 length), page data
where offsets are from the container's start and each page decodes to 4 KiB
(the last one zero-padded). A page whose length is 4096 is stored raw.
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lz4block  # noqa: E402

PAGE = 4096
LZ4_MARKER = ".lz4"
LZ4_MARKER_DATA = b"GZL4 romfs v1\n"


def lz4_container(data):
    pages = [data[i:i + PAGE] for i in range(0, len(data), PAGE)]
    head = 12 + 8 * len(pages)
    index, body = bytearray(), bytearray()
    for page in pages:
        page = page.ljust(PAGE, b"\0")
        packed = lz4block.compress(page)
        if len(packed) >= PAGE:
            packed = page                       # stored raw
        index += struct.pack("<II", head + len(body), len(packed))
        body += packed
    return b"GZL4" + struct.pack("<II", len(data), len(pages)) + bytes(index) + bytes(body)


def lz4_expand(blob):
    if blob[:4] != b"GZL4":
        raise ValueError("not an LZ4 romfs container")
    size, count = struct.unpack_from("<II", blob, 4)
    out = bytearray()
    for i in range(count):
        off, length = struct.unpack_from("<II", blob, 12 + 8 * i)
        chunk = blob[off:off + length]
        out += chunk if length == PAGE else lz4block.decompress(chunk, PAGE)
    return bytes(out[:size])


def entries(blob):
    """Every file's contents, by name, decompressed if the archive is."""
    if len(blob) < 8 or blob[:4] != b"GZRF":
        raise ValueError("not a Glaze romfs archive")
    count = struct.unpack_from("<I", blob, 4)[0]
    if count > (len(blob) - 8) // 56:
        raise ValueError("romfs has a truncated file table")
    files = {}
    for i in range(count):
        raw, offset, size = struct.unpack_from("<48sII", blob, 8 + 56 * i)
        name = raw.split(b"\0", 1)[0].decode("utf-8")
        if offset + size > len(blob):
            raise ValueError(f"romfs entry exceeds the archive: {name}")
        if name in files:
            raise ValueError(f"duplicate romfs entry: {name}")
        files[name] = blob[offset:offset + size]
    if LZ4_MARKER in files:
        del files[LZ4_MARKER]
        files = {name: lz4_expand(data) for name, data in files.items()}
    return files
