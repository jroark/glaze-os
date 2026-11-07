#!/usr/bin/env python3
"""Renders raster tiles from a local tileserver-gl into an MBTiles file for
tools/mkmaptiles.py, for the same --region layers.

Each 4x4 block of tiles is rendered as one image with a margin around it
(tileserver-gl's static endpoint), then cut into tiles, so labels are placed
across the block instead of being clipped at every tile edge.

    tools/osm/render_mbtiles.py --server http://127.0.0.1:8080 --style glaze \\
        --region 37.20,-122.60,38.10,-121.70:9-12 --region ...:13-16 --out out/osm/bay-raster.mbtiles
"""
import argparse, math, os, sqlite3, struct, sys, urllib.request, zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mkmaptiles  # noqa: E402  (tiles_in, png_decode)

BLOCK = 4           # tiles per side rendered together
MARGIN = 64         # pixels rendered around a block for label placement


def lon_of(px, z):
    return px / (256 << z) * 360 - 180


def lat_of(py, z):
    return math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * py / (256 << z)))))


def png_rgb(w, h, rows):
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    raw = b"".join(b"\0" + bytes(v for c in r for v in c) for r in rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default="http://127.0.0.1:8080")
    ap.add_argument("--style", default="glaze")
    ap.add_argument("--region", action="append", required=True, help="south,west,north,east:min-max")
    ap.add_argument("--attribution", default="© OpenStreetMap contributors")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    wanted = set()
    for r in args.region:
        box, _, zr = r.partition(":")
        b = tuple(float(v) for v in box.split(","))
        lo, _, hi = zr.partition("-")
        for z in range(int(lo), int(hi or lo) + 1):
            wanted |= {(z, x, y) for x, y in mkmaptiles.tiles_in(b, z)}
    blocks = sorted({(z, x // BLOCK, y // BLOCK) for z, x, y in wanted})
    print(f"{len(wanted)} tiles in {len(blocks)} blocks", file=sys.stderr)

    if os.path.exists(args.out):
        os.unlink(args.out)
    db = sqlite3.connect(args.out)
    db.execute("CREATE TABLE metadata (name text, value text)")
    db.execute("CREATE TABLE tiles (zoom_level integer, tile_column integer, tile_row integer, tile_data blob)")
    lats = [float(r.split(",")[0]) for r in args.region] + [float(r.split(",")[2]) for r in args.region]
    lons = [float(r.split(",")[1]) for r in args.region] + [float(r.split(",")[3].split(":")[0]) for r in args.region]
    db.executemany("INSERT INTO metadata VALUES (?, ?)", [
        ("name", Path(args.out).stem), ("format", "png"), ("attribution", args.attribution),
        ("bounds", f"{min(lons)},{min(lats)},{max(lons)},{max(lats)}")])

    for n, (z, bx, by) in enumerate(blocks, 1):
        tiles = 1 << z
        x0, y0 = bx * BLOCK, by * BLOCK
        cols, rows = min(BLOCK, tiles - x0), min(BLOCK, tiles - y0)
        # Pixel window with the margin, clamped to the world.
        px0, py0 = max(0, x0 * 256 - MARGIN), max(0, y0 * 256 - MARGIN)
        px1 = min(tiles * 256, (x0 + cols) * 256 + MARGIN)
        py1 = min(tiles * 256, (y0 + rows) * 256 + MARGIN)
        w, h = px1 - px0, py1 - py0
        bbox = f"{lon_of(px0, z)},{lat_of(py1, z)},{lon_of(px1, z)},{lat_of(py0, z)}"
        url = f"{args.server}/styles/{args.style}/static/{bbox}/{w}x{h}.png?padding=0"
        with urllib.request.urlopen(url, timeout=120) as r:
            iw, ih, px = mkmaptiles.png_decode(r.read())
        if (iw, ih) != (w, h):
            sys.exit(f"server rendered {iw}x{ih} for {w}x{h}")
        for tx in range(x0, x0 + cols):
            for ty in range(y0, y0 + rows):
                if (z, tx, ty) not in wanted:
                    continue
                ox, oy = tx * 256 - px0, ty * 256 - py0
                tile_rows = [px[(oy + j) * w + ox:(oy + j) * w + ox + 256] for j in range(256)]
                db.execute("INSERT INTO tiles VALUES (?, ?, ?, ?)",
                           (z, tx, tiles - 1 - ty, png_rgb(256, 256, tile_rows)))
        if n % 5 == 0 or n == len(blocks):
            db.commit()
            print(f"  {n}/{len(blocks)} blocks", file=sys.stderr)
    db.commit()
    print(f"{args.out}: {os.path.getsize(args.out) / 1048576:.1f} MiB", file=sys.stderr)


if __name__ == "__main__":
    main()
