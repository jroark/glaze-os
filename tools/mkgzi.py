#!/usr/bin/env python3
"""Convert a PNG to a GZI image (lib/include/glaze/gfx.h): RGB565, ordered
dithered like tools/mkassets.swift, plus an A8 plane when the PNG has
transparency. For app packages' icons; the OS artwork comes from
mkassets.swift. Needs Pillow.

usage: mkgzi.py IN.png OUT.gzi
"""

import struct
import sys

from PIL import Image

BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def main() -> int:
    img = Image.open(sys.argv[1]).convert("RGBA")
    w, h = img.size
    data = img.load()
    alpha = any(data[x, y][3] < 255 for y in range(h) for x in range(w))
    px, al = bytearray(), bytearray()
    for y in range(h):
        for x in range(w):
            r, g, b, a = data[x, y]
            t = BAYER[y & 3][x & 3]
            r5 = min(31, (r + (t >> 1)) >> 3)
            g6 = min(63, (g + (t >> 2)) >> 2)
            b5 = min(31, (b + (t >> 1)) >> 3)
            px += struct.pack("<H", (r5 << 11) | (g6 << 5) | b5)
            al.append(a)
    out = b"GZI1" + struct.pack("<HHHH", w, h, 1 if alpha else 0, 0) + px + (al if alpha else b"")
    open(sys.argv[2], "wb").write(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
