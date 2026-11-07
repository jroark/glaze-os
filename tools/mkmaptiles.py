#!/usr/bin/env python3
"""Builds a Glaze map tile pack (.gzmap) for the Maps app, from an MBTiles
file or a tile server, for a bounding box and zoom range.

    tools/mkmaptiles.py --mbtiles city.mbtiles --bbox 37.70,-122.52,37.83,-122.35 \\
        --zoom 10-16 --out out/maps/sf.gzmap
    tools/mkmaptiles.py --mbtiles city.mbtiles --region 37.2,-122.6,38.1,-121.7:9-12 \\
        --region 37.70,-122.52,37.83,-122.35:13-15 --out out/maps/bay.gzmap
    tools/mkmaptiles.py --url 'https://tiles.example.com/{z}/{x}/{y}.png' \\
        --user-agent 'glaze-mkmaptiles (you@example.com)' --attribution '(c) OpenStreetMap contributors' \\
        --bbox ... --zoom 10-15 --out out/maps/sf.gzmap

Use a source whose terms allow offline copies: an MBTiles you rendered or
downloaded, or a provider that permits it. tile.openstreetmap.org forbids
bulk downloading. Downloads are cached in out/tilecache and rate-limited.

Format (little-endian), read by glaze-apps packages/maps:
  header (64 bytes): "GZMP", u32 version 1, u16 tile size 256, u8 min zoom,
    u8 max zoom, u32 tile count, u32 index offset, u32 attribution offset,
    u32 attribution length, i32 south, west, north, east (1e-7 degrees),
    u32 name offset, u32 name length, zero to 64.
  index: tile count entries of u32 (zoom << 24 | x), u32 y (XYZ, y down),
    u32 offset, u32 length, sorted by the first two words.
  tile: u16 colours (1-256), u16 0, colours x u16 RGB565, then a raw
    deflate stream of 256*256 palette indices, row by row. Identical
    tiles share one blob.
"""
import argparse, hashlib, io, math, os, sqlite3, struct, sys, time, urllib.request, zlib

TILE = 256


# --- PNG decoding (8-bit and palette PNGs, no interlace) without Pillow ---

def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else (b if pb <= pc else c)


def png_decode(data):
    """Returns (width, height, list of (r, g, b) rows flattened)."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, plte, trns = 8, b"", None, None
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            plte = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        elif kind == b"tRNS":
            trns = body
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    if interlace:
        raise ValueError("interlaced PNG: install Pillow")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    if depth != 8 and not (ctype in (0, 3) and depth in (1, 2, 4)):
        raise ValueError(f"PNG depth {depth}: install Pillow")
    raw = zlib.decompress(idat)
    bpp = max(1, channels * depth // 8)
    stride = (w * channels * depth + 7) // 8
    out, prev, p = [], bytearray(stride), 0
    for _ in range(h):
        f = raw[p]; line = bytearray(raw[p + 1:p + 1 + stride]); p += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4: line[i] = (line[i] + _paeth(a, b, c)) & 255
        prev = line
        if depth < 8:
            per = 8 // depth
            vals = [(line[x // per] >> (8 - depth * (x % per + 1))) & ((1 << depth) - 1) for x in range(w)]
        else:
            vals = None
        for x in range(w):
            if ctype == 3:
                k = vals[x] if vals else line[x]
                r, g, b_ = plte[k]
                alpha = trns[k] if trns and k < len(trns) else 255
            elif ctype == 0:
                v = vals[x] * 255 // ((1 << depth) - 1) if vals else line[x]
                r = g = b_ = v; alpha = 255
            elif ctype == 4:
                r = g = b_ = line[2 * x]; alpha = line[2 * x + 1]
            elif ctype == 2:
                r, g, b_ = line[3 * x:3 * x + 3]; alpha = 255
            else:
                r, g, b_, alpha = line[4 * x:4 * x + 4]
            if alpha < 255:     # over white, as a map's background
                r = (r * alpha + 255 * (255 - alpha)) // 255
                g = (g * alpha + 255 * (255 - alpha)) // 255
                b_ = (b_ * alpha + 255 * (255 - alpha)) // 255
            out.append((r, g, b_))
    return w, h, out


def decode(data):
    try:
        from PIL import Image
        im = Image.open(io.BytesIO(data)).convert("RGBA")
        bg = Image.new("RGBA", im.size, (255, 255, 255, 255))
        im = Image.alpha_composite(bg, im).convert("RGB")
        return im.size[0], im.size[1], list(im.getdata())
    except ImportError:
        return png_decode(data)


def downscale(w, h, px):
    """Box-filters a 512-pixel (retina) tile to 256."""
    if (w, h) == (TILE, TILE):
        return px
    if (w, h) != (2 * TILE, 2 * TILE):
        raise ValueError(f"{w}x{h} tile: only 256 and 512 are supported")
    out = []
    for y in range(TILE):
        for x in range(TILE):
            a, b = px[2 * y * w + 2 * x], px[2 * y * w + 2 * x + 1]
            c, d = px[(2 * y + 1) * w + 2 * x], px[(2 * y + 1) * w + 2 * x + 1]
            out.append(tuple((a[i] + b[i] + c[i] + d[i] + 2) // 4 for i in range(3)))
    return out


def rgb565(c):
    r, g, b = c
    return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3


def encode(px):
    """RGB pixels to a tile blob: the 256 most common RGB565 colours, the
    rest mapped to the nearest of them."""
    colours = [rgb565(c) for c in px]
    counts = {}
    for c in colours:
        counts[c] = counts.get(c, 0) + 1
    palette = sorted(counts, key=lambda c: -counts[c])[:256]
    index = {c: i for i, c in enumerate(palette)}
    if len(counts) > 256:
        def split(c): return ((c >> 11) << 3, ((c >> 5) & 63) << 2, (c & 31) << 3)
        pal_rgb = [split(c) for c in palette]
        for c in counts:
            if c not in index:
                r, g, b = split(c)
                index[c] = min(range(len(pal_rgb)), key=lambda i: (pal_rgb[i][0] - r) ** 2 * 3 +
                               (pal_rgb[i][1] - g) ** 2 * 4 + (pal_rgb[i][2] - b) ** 2 * 2)
    pixels = bytes(index[c] for c in colours)
    z = zlib.compressobj(9, zlib.DEFLATED, -15)
    body = z.compress(pixels) + z.flush()
    return struct.pack("<HH", len(palette), 0) + struct.pack(f"<{len(palette)}H", *palette) + body


# --- tile coordinates ---

def tile_xy(lat, lon, z):
    lat = max(-85.0511287798, min(85.0511287798, lat))
    n = 1 << z
    x = int((lon + 180) / 360 * n)
    y = int((1 - math.log(math.tan(math.radians(lat)) + 1 / math.cos(math.radians(lat))) / math.pi) / 2 * n)
    return min(max(x, 0), n - 1), min(max(y, 0), n - 1)


def tiles_in(bbox, z):
    s, w, n, e = bbox
    x0, y0 = tile_xy(n, w, z)
    x1, y1 = tile_xy(s, e, z)
    for x in range(x0, x1 + 1):
        for y in range(y0, y1 + 1):
            yield x, y


# --- sources ---

class MBTiles:
    def __init__(self, path):
        self.db = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
        self.meta = dict(self.db.execute("SELECT name, value FROM metadata").fetchall())

    def get(self, z, x, y):
        row = self.db.execute("SELECT tile_data FROM tiles WHERE zoom_level=? AND tile_column=? AND tile_row=?",
                              (z, x, (1 << z) - 1 - y)).fetchone()     # MBTiles rows are TMS: y up
        return row[0] if row else None

    def bbox(self):
        if "bounds" in self.meta:
            w, s, e, n = (float(v) for v in self.meta["bounds"].split(","))
            return s, w, n, e
        return None


class Server:
    def __init__(self, template, user_agent, delay, cache):
        self.template, self.user_agent, self.delay, self.cache = template, user_agent, delay, cache
        self.last = 0.0

    def get(self, z, x, y):
        key = hashlib.sha1(self.template.encode()).hexdigest()[:12]
        path = os.path.join(self.cache, key, str(z), str(x), f"{y}.tile")
        if os.path.exists(path):
            return open(path, "rb").read()
        wait = self.last + self.delay - time.time()
        if wait > 0:
            time.sleep(wait)
        url = self.template.format(z=z, x=x, y=y)
        req = urllib.request.Request(url, headers={"User-Agent": self.user_agent})
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                data = r.read()
        except urllib.error.HTTPError as err:
            if err.code == 404:
                return None
            raise
        finally:
            self.last = time.time()
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, "wb").write(data)
        return data


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--mbtiles", help="an MBTiles file (raster PNG or JPEG tiles)")
    src.add_argument("--url", help="tile URL template with {z}, {x}, {y}")
    ap.add_argument("--bbox", help="south,west,north,east in degrees (default: the MBTiles bounds)")
    ap.add_argument("--zoom", default="10-15", help="min-max zoom (default 10-15)")
    ap.add_argument("--region", action="append", default=[],
                    help="south,west,north,east:min-max, repeatable: coarse zooms over a wide area, "
                         "deep ones over a smaller one (replaces --bbox and --zoom)")
    ap.add_argument("--out", required=True, help="the .gzmap to write")
    ap.add_argument("--name", help="pack name shown in Maps (default: the file name)")
    ap.add_argument("--attribution", help="attribution text (default: the MBTiles attribution)")
    ap.add_argument("--user-agent", help="for --url: identify yourself to the tile server")
    ap.add_argument("--delay", type=float, default=0.5, help="for --url: seconds between requests")
    ap.add_argument("--cache", default="out/tilecache", help="for --url: download cache")
    ap.add_argument("--budget", type=float, default=4.0, help="MiB the pack may take (default 4)")
    args = ap.parse_args()

    def zooms(text):
        lo, _, hi = text.partition("-")
        lo, hi = int(lo), int(hi or lo)
        if not 0 <= lo <= hi <= 20:
            sys.exit("zoom must be within 0-20")
        return lo, hi
    regions = []
    for r in args.region:
        box, _, zr = r.partition(":")
        b = tuple(float(v) for v in box.split(","))
        if len(b) != 4 or not zr:
            sys.exit(f"--region {r}: expected south,west,north,east:min-max")
        regions.append((b, zooms(zr)))
    if args.mbtiles:
        source = MBTiles(args.mbtiles)
        attribution = args.attribution or source.meta.get("attribution", "")
        bbox = tuple(float(v) for v in args.bbox.split(",")) if args.bbox else source.bbox()
    else:
        if not args.user_agent:
            sys.exit("--url needs --user-agent naming you or your app, as tile servers' policies require")
        if not args.attribution:
            sys.exit("--url needs --attribution: the map's licence requires it on screen")
        source = Server(args.url, args.user_agent, args.delay, args.cache)
        attribution = args.attribution
        bbox = tuple(float(v) for v in args.bbox.split(",")) if args.bbox else None
    if not regions:
        if not bbox or len(bbox) != 4:
            sys.exit("--bbox south,west,north,east (or --region) is required")
        regions = [(bbox, zooms(args.zoom))]
    bbox = (min(b[0] for b, _ in regions), min(b[1] for b, _ in regions),
            max(b[2] for b, _ in regions), max(b[3] for b, _ in regions))
    zmin, zmax = min(z[0] for _, z in regions), max(z[1] for _, z in regions)
    name = args.name or os.path.splitext(os.path.basename(args.out))[0]

    wanted = sorted({(z, x, y) for b, (lo, hi) in regions for z in range(lo, hi + 1) for x, y in tiles_in(b, z)})
    print(f"{len(wanted)} tiles at zoom {zmin}-{zmax}", file=sys.stderr)
    if not args.mbtiles and len(wanted) > 5000:
        sys.exit("more than 5000 tiles to download: narrow --bbox or --zoom")

    blobs, entries, size, missing = {}, [], 0, 0
    budget = int(args.budget * 1024 * 1024)
    for n, (z, x, y) in enumerate(wanted, 1):
        data = source.get(z, x, y)
        if not data:
            missing += 1
            continue
        w, h, px = decode(data)
        blob = encode(downscale(w, h, px))
        digest = hashlib.sha1(blob).digest()
        if digest not in blobs:
            blobs[digest] = blob
            size += len(blob)
        entries.append((z << 24 | x, y, digest))
        if n % 50 == 0:
            print(f"  {n}/{len(wanted)}, {size / 1048576:.2f} MiB", file=sys.stderr)
        if size > budget:
            # Keep the whole zoom levels that fit; tiles are fetched shallow to deep.
            if z == zmin:
                sys.exit(f"zoom {z} alone is over the {args.budget} MiB budget: narrow the area or raise --budget")
            entries = [e for e in entries if e[0] >> 24 < z]
            keep = {e[2] for e in entries}
            blobs = {d: b for d, b in blobs.items() if d in keep}
            zmax = z - 1
            print(f"over the {args.budget} MiB budget at zoom {z}: keeping zoom {zmin}-{zmax}", file=sys.stderr)
            break

    entries.sort()
    attr_b, name_b = attribution.encode()[:255], name.encode()[:63]
    header_size, index_size = 64, 16 * len(entries)
    attr_off = header_size + index_size
    name_off = attr_off + len(attr_b) + 1
    data_off = (name_off + len(name_b) + 1 + 3) & ~3
    offsets, out_blobs, pos = {}, [], data_off
    for zx, y, digest in entries:
        if digest not in offsets:
            offsets[digest] = pos
            out_blobs.append(blobs[digest])
            pos += (len(blobs[digest]) + 3) & ~3
    s, w, nn, e = bbox
    header = struct.pack("<4sIHBBIIIIiiiiII", b"GZMP", 1, TILE, zmin, zmax, len(entries), header_size,
                         attr_off, len(attr_b), round(s * 1e7), round(w * 1e7), round(nn * 1e7), round(e * 1e7),
                         name_off, len(name_b))
    header += bytes(header_size - len(header))
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(header)
        for zx, y, digest in entries:
            f.write(struct.pack("<IIII", zx, y, offsets[digest], len(blobs[digest])))
        f.write(attr_b + b"\0" + name_b + b"\0")
        f.write(bytes(data_off - f.tell()))
        for blob in out_blobs:
            f.write(blob + bytes(((len(blob) + 3) & ~3) - len(blob)))
    total = os.path.getsize(args.out)
    print(f"zoom {zmin}-{zmax}", file=sys.stderr)
    print(f"{args.out}: {len(entries)} tiles ({len(blobs)} distinct, {missing} missing), "
          f"{total / 1048576:.2f} MiB of a {args.budget} MiB budget", file=sys.stderr)


if __name__ == "__main__":
    main()
