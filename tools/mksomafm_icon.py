#!/usr/bin/env python3
"""Draw the original SomaFM package radio icon; standard-library only."""
from pathlib import Path
import math
import struct
import zlib


def rounded(x, y, rect, radius):
    left, top, right, bottom = rect
    if not left <= x <= right or not top <= y <= bottom:
        return False
    cx = min(max(x, left + radius), right - radius)
    cy = min(max(y, top + radius), bottom - radius)
    return (x-cx)**2 + (y-cy)**2 <= radius**2


def pixel(x, y):
    color = (0, 0, 0, 0)
    if rounded(x, y, (0, 0, 44, 44), 8):
        color = (int(51-y*.7), int(83-y*.8), int(112-y*.9), 255)
    # Telescoping antenna.
    dx, dy = 18, -9
    t = max(0, min(1, ((x-12)*dx + (y-15)*dy) / (dx*dx+dy*dy)))
    if math.hypot(x-(12+t*dx), y-(15+t*dy)) < .85:
        color = (227, 238, 248, 255)
    if rounded(x, y, (6, 14, 38, 36), 4):
        color = (7, 18, 29, 255)
    if rounded(x, y, (7, 15, 37, 35), 3):
        color = (int(230-y*.35), int(239-y*.3), int(244-y*.2), 255)
    if rounded(x, y, (10, 18, 27, 24), 1):
        color = (88, 169, 150, 255)
        if 19 < y < 23 and int(x*2) % 6 == 0:
            color = (29, 83, 73, 255)
    if math.hypot(x-32, y-21) < 2.7:
        color = (178, 55, 49, 255)
    if 10 < x < 26 and any(abs(y-line) < .45 for line in (27, 29, 31, 33)):
        color = (73, 96, 112, 255)
    if math.hypot(x-32, y-30) < 2.7:
        color = (51, 72, 89, 255)
    if math.hypot(x-31.5, y-29.5) < 1.8:
        color = (94, 117, 133, 255)
    return color


def main():
    target = Path(__file__).resolve().parents[1] / 'packages/somafm'
    pixels = []
    for y in range(44):
        for x in range(44):
            samples = [pixel(x+(i+.5)/4, y+(j+.5)/4) for j in range(4) for i in range(4)]
            alpha = sum(p[3] for p in samples)
            pixels.append(tuple(round(sum(p[c]*p[3] for p in samples)/alpha) if alpha else 0
                                for c in range(3)) + (round(alpha/16),))
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind+data))
    raw = b''.join(b'\0' + bytes(v for p in pixels[y*44:(y+1)*44] for v in p) for y in range(44))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB',44,44,8,6,0,0,0))
    png += chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
    (target/'icon.png').write_bytes(png)
    rgb = b''.join(struct.pack('<H', (r>>3)<<11 | (g>>2)<<5 | b>>3) for r,g,b,a in pixels)
    (target/'icon.gzi').write_bytes(b'GZI1' + struct.pack('<HHHH',44,44,1,0) + rgb + bytes(p[3] for p in pixels))


if __name__ == '__main__':
    main()
