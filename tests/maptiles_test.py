#!/usr/bin/env python3
"""tools/mkmaptiles.py round trip: a synthetic MBTiles (palette, RGB and
RGBA PNGs drawn here) to a .gzmap, then every tile decoded back from the
pack and compared with its source pixels in RGB565.

    python3 tests/maptiles_test.py [--keep DIR]

With --keep, DIR/demo.mbtiles and DIR/demo.gzmap stay behind: a small
synthetic street grid around tests/gps/track.nmea, for the Maps app's
emulator tests (no tile server needed).
"""
import math, os, sqlite3, struct, subprocess, sys, tempfile, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "tools", "mkmaptiles.py")
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import mkmaptiles  # noqa: E402

BBOX = (37.7900, -122.4010, 37.8010, -122.3860)   # around tests/gps/track.nmea


def png(w, h, rows, ctype, palette=None):
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    raw = b"".join(b"\x00" + bytes(r) for r in rows)
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, ctype, 0, 0, 0))
    if palette:
        out += chunk(b"PLTE", bytes(c for rgb in palette for c in rgb))
    return out + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


def street_tile(z, x, y, kind):
    """A map-like tile: land, blocks of streets every ~80 m, a park, water."""
    land, road, park, water, edge = (242, 239, 233), (255, 255, 255), (200, 230, 190), (170, 211, 223), (210, 205, 195)
    n = 1 << z
    px = []
    for j in range(256):
        lat = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * (y + j / 256) / n))))
        row = []
        for i in range(256):
            lon = (x + i / 256) / n * 360 - 180
            gx, gy = (lon + 122.4) * 1100, (lat - 37.79) * 1400
            fx, fy = gx - math.floor(gx), gy - math.floor(gy)
            c = land
            if lon > -122.389:
                c = water
            elif 37.7935 < lat < 37.7965 and -122.398 < lon < -122.394:
                c = park
            elif fx < 0.12 or fy < 0.12:
                c = road
            elif fx < 0.15 or fy < 0.15:
                c = edge
            row.append(c)
        px.append(row)
    if kind == "palette":
        pal = sorted({c for r in px for c in r})
        return png(256, 256, [[pal.index(c) for c in r] for r in px], 3, pal)
    if kind == "rgba":
        return png(256, 256, [[v for c in r for v in c + (255,)] for r in px], 6)
    return png(256, 256, [[v for c in r for v in c] for r in px], 2)


def make_mbtiles(path, zooms):
    db = sqlite3.connect(path)
    db.execute("CREATE TABLE metadata (name text, value text)")
    db.execute("CREATE TABLE tiles (zoom_level integer, tile_column integer, tile_row integer, tile_data blob)")
    db.executemany("INSERT INTO metadata VALUES (?, ?)", [
        ("name", "demo"), ("format", "png"), ("attribution", "Synthetic test map"),
        ("bounds", f"{BBOX[1]},{BBOX[0]},{BBOX[3]},{BBOX[2]}")])
    kinds = ["palette", "rgb", "rgba"]
    count = 0
    for z in zooms:
        for x, y in mkmaptiles.tiles_in(BBOX, z):
            db.execute("INSERT INTO tiles VALUES (?, ?, ?, ?)",
                       (z, x, (1 << z) - 1 - y, street_tile(z, x, y, kinds[count % 3])))
            count += 1
    db.commit()
    return count


def read_pack(path):
    data = open(path, "rb").read()
    magic, version, size, zmin, zmax, count, index_off, attr_off, attr_len, s, w, n, e, name_off, name_len = \
        struct.unpack_from("<4sIHBBIIIIiiiiII", data)
    assert magic == b"GZMP" and version == 1 and size == 256
    tiles = {}
    prev = None
    for k in range(count):
        zx, y, off, length = struct.unpack_from("<IIII", data, index_off + 16 * k)
        assert prev is None or (zx, y) > prev, "index sorted"
        prev = (zx, y)
        colours, _ = struct.unpack_from("<HH", data, off)
        pal = struct.unpack_from(f"<{colours}H", data, off + 4)
        pixels = zlib.decompress(data[off + 4 + 2 * colours:off + length], -15)
        assert len(pixels) == 65536
        tiles[(zx >> 24, zx & 0xFFFFFF, y)] = [pal[p] for p in pixels]
    attribution = data[attr_off:attr_off + attr_len].decode()
    name = data[name_off:name_off + name_len].decode()
    return (zmin, zmax, (s, w, n, e), attribution, name), tiles


def main():
    keep = sys.argv[2] if len(sys.argv) > 2 and sys.argv[1] == "--keep" else None
    work = keep or tempfile.mkdtemp()
    os.makedirs(work, exist_ok=True)
    mb, pack = os.path.join(work, "demo.mbtiles"), os.path.join(work, "demo.gzmap")
    if os.path.exists(mb):
        os.unlink(mb)
    zooms = range(14, 18)
    count = make_mbtiles(mb, zooms)
    subprocess.run([sys.executable, "-I", TOOL, "--mbtiles", mb, "--zoom", "14-17", "--out", pack],
                   check=True)
    (zmin, zmax, bounds, attribution, name), tiles = read_pack(pack)
    assert (zmin, zmax) == (14, 17) and attribution == "Synthetic test map" and name == "demo"
    assert bounds == tuple(round(v * 1e7) for v in BBOX)
    assert len(tiles) == count
    db = sqlite3.connect(mb)
    for z, x, tms, blob in db.execute("SELECT zoom_level, tile_column, tile_row, tile_data FROM tiles"):
        y = (1 << z) - 1 - tms
        w, h, px = mkmaptiles.png_decode(blob)
        expect = [mkmaptiles.rgb565(c) for c in px]
        assert tiles[(z, x, y)] == expect, f"tile {z}/{x}/{y}"
    print(f"maptiles: {count} tiles (palette, RGB, RGBA PNGs) round-trip exactly, "
          f"{os.path.getsize(pack)} bytes PASS")


if __name__ == "__main__":
    main()
