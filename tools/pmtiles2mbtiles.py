#!/usr/bin/env python3
"""Copies a bounding box of tiles out of a PMTiles v3 archive, local or over
HTTP range requests, into an MBTiles file. Standard library only.

    tools/pmtiles2mbtiles.py https://build.protomaps.com/20261106.pmtiles \\
        --bbox 37.15,-122.65,38.15,-121.65 --maxzoom 15 --out out/osm/bay.mbtiles

Used for Protomaps' OpenStreetMap basemap builds (ODbL: credit "OpenStreetMap
contributors"), which tools/osm renders for tools/mkmaptiles.py. Tiles are
copied as they are stored (gzipped MVT); nearby ranges are fetched together.
PMTiles spec: https://github.com/protomaps/PMTiles/blob/main/spec/v3/spec.md
"""
import argparse, gzip, json, math, os, sqlite3, struct, sys, urllib.request

COMPRESSION = {0: "unknown", 1: "none", 2: "gzip", 3: "brotli", 4: "zstd"}


class Source:
    def __init__(self, where):
        self.where = where
        self.remote = where.startswith(("http://", "https://"))
        self.file = None if self.remote else open(where, "rb")
        self.requests = 0

    def read(self, offset, length):
        self.requests += 1
        if not self.remote:
            self.file.seek(offset)
            return self.file.read(length)
        req = urllib.request.Request(self.where, headers={
            "Range": f"bytes={offset}-{offset + length - 1}", "User-Agent": "glaze-pmtiles2mbtiles/1.0"})
        for attempt in range(4):
            try:
                with urllib.request.urlopen(req, timeout=60) as r:
                    data = r.read()
                if r.status != 206 and length < 1 << 30:
                    raise OSError(f"server ignored the range request (HTTP {r.status})")
                if len(data) != length:
                    raise OSError(f"short read: {len(data)} of {length}")
                return data
            except OSError:
                if attempt == 3:
                    raise


def decompress(data, kind):
    if kind in (0, 1):
        return data
    if kind == 2:
        return gzip.decompress(data)
    raise ValueError(f"{COMPRESSION.get(kind, kind)} compression is not supported (stdlib has gzip only)")


def varints(data):
    pos = 0
    while pos < len(data):
        shift = value = 0
        while True:
            b = data[pos]; pos += 1
            value |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                break
        yield value


def parse_directory(data):
    it = varints(data)
    n = next(it)
    ids, last = [], 0
    for _ in range(n):
        last += next(it); ids.append(last)
    runs = [next(it) for _ in range(n)]
    lengths = [next(it) for _ in range(n)]
    offsets = []
    for i in range(n):
        v = next(it)
        offsets.append(offsets[i - 1] + lengths[i - 1] if v == 0 and i > 0 else v - 1)
    return list(zip(ids, runs, offsets, lengths))


def zxy_to_tileid(z, x, y):
    """Hilbert curve order within a zoom, after all lower zooms' tiles."""
    acc = ((1 << (2 * z)) - 1) // 3
    n = 1 << z
    s = n >> 1
    while s > 0:
        rx = 1 if x & s else 0
        ry = 1 if y & s else 0
        acc += s * s * ((3 * rx) ^ ry)
        if ry == 0:
            if rx == 1:
                x, y = n - 1 - x, n - 1 - y
            x, y = y, x
        s >>= 1
    return acc


def tileid_to_zxy(t):
    z = 0
    while t >= ((1 << (2 * (z + 1))) - 1) // 3:
        z += 1
    d = t - ((1 << (2 * z)) - 1) // 3
    x = y = 0
    s = 1
    n = 1 << z
    while s < n:
        rx = 1 & (d // 2)
        ry = 1 & (d ^ rx)
        if ry == 0:
            if rx == 1:
                x, y = s - 1 - x, s - 1 - y
            x, y = y, x
        x += s * rx
        y += s * ry
        d //= 4
        s *= 2
    return z, x, y


def tiles_in(bbox, z):
    s, w, n, e = bbox
    def xy(lat, lon):
        lat = max(-85.0511287798, min(85.0511287798, lat))
        k = 1 << z
        x = int((lon + 180) / 360 * k)
        y = int((1 - math.log(math.tan(math.radians(lat)) + 1 / math.cos(math.radians(lat))) / math.pi) / 2 * k)
        return min(max(x, 0), k - 1), min(max(y, 0), k - 1)
    x0, y0 = xy(n, w)
    x1, y1 = xy(s, e)
    return [(x, y) for x in range(x0, x1 + 1) for y in range(y0, y1 + 1)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help="a .pmtiles path or https URL")
    ap.add_argument("--bbox", required=True, help="south,west,north,east")
    ap.add_argument("--minzoom", type=int, default=0)
    ap.add_argument("--maxzoom", type=int, default=15)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    bbox = tuple(float(v) for v in args.bbox.split(","))
    assert all(zxy_to_tileid(*tileid_to_zxy(t)) == t for t in range(0, 30000, 7)), "Hilbert self-check"

    src = Source(args.source)
    h = src.read(0, 127)
    if h[:7] != b"PMTiles" or h[7] != 3:
        sys.exit("not a PMTiles v3 archive")
    (root_off, root_len, meta_off, meta_len, leaf_off, leaf_len, data_off, data_len,
     _addressed, _entries, _contents) = struct.unpack_from("<11Q", h, 8)
    _clustered, internal, tile_comp, tile_type, minz, maxz = struct.unpack_from("<6B", h, 96)
    print(f"PMTiles: zoom {minz}-{maxz}, type {tile_type}, tiles {COMPRESSION.get(tile_comp)}, "
          f"directories {COMPRESSION.get(internal)}", file=sys.stderr)
    metadata = json.loads(decompress(src.read(meta_off, meta_len), internal) or b"{}")
    root = parse_directory(decompress(src.read(root_off, root_len), internal))
    leaves = {}

    def find(tid, entries):
        lo, hi = 0, len(entries) - 1
        best = -1
        while lo <= hi:
            mid = (lo + hi) // 2
            if entries[mid][0] <= tid:
                best = mid; lo = mid + 1
            else:
                hi = mid - 1
        if best < 0:
            return None
        start, run, off, length = entries[best]
        if run == 0:
            key = (off, length)
            if key not in leaves:
                leaves[key] = parse_directory(decompress(src.read(leaf_off + off, length), internal))
            return find(tid, leaves[key])
        return (off, length) if tid < start + run else None

    zmax = min(args.maxzoom, maxz)
    wanted = {}
    for z in range(max(args.minzoom, minz), zmax + 1):
        for x, y in tiles_in(bbox, z):
            hit = find(zxy_to_tileid(z, x, y), root)
            if hit:
                wanted.setdefault(hit, []).append((z, x, y))
    print(f"{sum(len(v) for v in wanted.values())} tiles ({len(wanted)} distinct) at zoom "
          f"{args.minzoom}-{zmax}; {len(leaves)} leaf directories", file=sys.stderr)

    if os.path.exists(args.out):
        os.unlink(args.out)
    db = sqlite3.connect(args.out)
    db.execute("CREATE TABLE metadata (name text, value text)")
    db.execute("CREATE TABLE tiles (zoom_level integer, tile_column integer, tile_row integer, tile_data blob)")
    db.execute("CREATE UNIQUE INDEX tile_index ON tiles (zoom_level, tile_column, tile_row)")
    s, w, n, e = bbox
    meta = {"name": metadata.get("name", "pmtiles extract"), "format": "pbf" if tile_type == 1 else "png",
            "minzoom": str(args.minzoom), "maxzoom": str(zmax), "bounds": f"{w},{s},{e},{n}",
            "attribution": metadata.get("attribution", "© OpenStreetMap contributors"),
            "json": json.dumps({k: metadata[k] for k in ("vector_layers",) if k in metadata})}
    db.executemany("INSERT INTO metadata VALUES (?, ?)", meta.items())

    # Fetch nearby tiles in one request: merge ranges less than 256 KiB apart, up to 16 MiB.
    ranges = sorted(wanted)
    done = 0
    i = 0
    while i < len(ranges):
        start = ranges[i][0]
        j = i
        end = ranges[i][0] + ranges[i][1]
        while j + 1 < len(ranges) and ranges[j + 1][0] - end < 256 * 1024 and \
                ranges[j + 1][0] + ranges[j + 1][1] - start <= 16 << 20:
            j += 1
            end = max(end, ranges[j][0] + ranges[j][1])
        blob = src.read(data_off + start, end - start)
        for off, length in ranges[i:j + 1]:
            data = blob[off - start:off - start + length]
            for z, x, y in wanted[(off, length)]:
                db.execute("INSERT INTO tiles VALUES (?, ?, ?, ?)", (z, x, (1 << z) - 1 - y, data))
        done += j - i + 1
        print(f"  {done}/{len(ranges)} tile ranges, {src.requests} requests", file=sys.stderr)
        i = j + 1
    db.commit()
    print(f"{args.out}: {os.path.getsize(args.out) / 1048576:.1f} MiB", file=sys.stderr)


if __name__ == "__main__":
    main()
