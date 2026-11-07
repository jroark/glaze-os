# OpenStreetMap tile packs

Builds Maps' `.gzmap` packs from OpenStreetMap data, rendered locally. It needs
no API key, and the only credit is "© OpenStreetMap contributors" (ODbL). Don't
use tile.openstreetmap.org: its policy forbids bulk and offline downloads.
Everything lands under ignored `out/osm`, and generated packs are never
committed.

1. **Vector tiles for the area**, from a Protomaps OSM build (land and ocean
   polygons included), fetched with HTTP range requests. Today's build is listed
   at https://build-metadata.protomaps.dev/builds.json.

   ```sh
   tools/pmtiles2mbtiles.py https://build.protomaps.com/20261106.pmtiles \
       --bbox 37.15,-122.65,38.15,-121.65 --maxzoom 15 --out out/osm/bay-pm.mbtiles
   ```

2. **Render them** with `maptiler/tileserver-gl` (MapLibre Native) in Docker,
   using `glaze-style.json`. The style is made for a 240×320 RGB565 screen: flat
   colours that survive the 256-colour palette, bold roads and large labels. It
   is written against the Protomaps v4 schema and uses the image's bundled Noto
   Sans Regular font.

   ```sh
   cat > out/osm/config.json <<'JSON'
   {"options": {"paths": {"root": "/data", "styles": "/styles", "mbtiles": "/data",
                          "fonts": "/usr/src/app/node_modules/tileserver-gl-styles/fonts"}},
    "styles": {"glaze": {"style": "glaze-style.json"}},
    "data": {"bay": {"mbtiles": "bay-pm.mbtiles"}}}
   JSON
   docker run -d --name glaze-tiles -v "$PWD/out/osm":/data -v "$PWD/tools/osm":/styles:ro \
       -p 127.0.0.1:8080:8080 maptiler/tileserver-gl --config /data/config.json
   ```

   Restart the container after editing the style.

3. **Cut raster tiles** with `render_mbtiles.py`. It renders 4×4 blocks of
   tiles with a margin, so labels aren't clipped at every tile edge. Then pack
   them:

   ```sh
   R="--region 37.20,-122.60,38.10,-121.70:9-12 --region 37.70,-122.52,37.83,-122.35:13-15 \
      --region 37.765,-122.45,37.81,-122.385:16"
   tools/osm/render_mbtiles.py $R --out out/osm/bay-raster.mbtiles
   tools/mkmaptiles.py --mbtiles out/osm/bay-raster.mbtiles $R --name "San Francisco Bay" \
       --attribution "(c) OpenStreetMap contributors" --out out/maps/sf-bay-osm.gzmap
   ```

   `mkmaptiles.py` keeps the whole zoom levels that fit its budget (4 MiB by
   default), and Maps magnifies the deepest one.

Install the pack with `glaze-usb put out/maps/x.gzmap /Maps/x.gzmap`, or put
it in a NAND image's factory file system with `FS_EXTRA` (`docs/hardware/GPS.md`).

Geofabrik extracts with `tilemaker` work for land, but the oceans need
osmdata.openstreetmap.de's water polygons. Protomaps' builds already include
them.
