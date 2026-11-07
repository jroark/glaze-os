# CF GPS receivers

Glaze reads NMEA-0183 from a GPS receiver on a CompactFlash serial card. The
target card is the AmbiCom GPS-CF. The Maps app (glaze-apps `packages/maps`)
shows the position on offline tile packs, which `tools/mkmaptiles.py` builds
on a desktop.

## The card

AmbiCom never published a Windows CE driver for the GPS-CF. Its quick
installation guide (BT-GPS, GPS-CF, GPS-CF3, GPS-USB) says only to insert the
card and pick its COM port in the mapping software. Its product sheet lists:

- NMEA-0183 (GGA, GSA, GSV, RMC) and SiRF binary;
- 12 channels;
- hot, warm and cold starts of 5, 36 and 45 s at most.

That is a SiRF receiver behind a plain 16550 UART. Similar SiRF CF cards
(GlobalSat BC-337, Leadtek 9534, Holux GM-270) run at 4800 baud 8N1 through
Windows CE's generic CF serial driver. So the card needs no vendor driver: only
the CIS match, the UART configuration and the NMEA stream.

**CIS**, read on a BE-300 with `glaze-usb bt-info` on 2026-11-06:

- VERS_1 `CFGPS` `LPV1.0`, MANFID `0104:0060`, FUNCID `02` (serial,
  initialised at POST).
- One serial FUNCE, type 0: a 16550 UART, no modem functions.
- CONFIG: COR at attribute `0x400`, last index `0x2d`, mask `03`.
- CFTABLE entries `0x0d`, `0x15`, `0x1d` and `0x25` decode 3f8h, 2f8h, 3e8h and
  2e8h, eight 8-bit ports each. Their subtuples name them COM1-COM4. Entry
  `0x2d` (`COMX`) takes any base on 16 address lines.

The generic path takes it: Glaze writes COR `0x4d` (entry `0x0d`, level
interrupts) and the 16550 is at 3f8h. `cf-boot-test` checks this against the
captured bytes, and be300's `--cf-gps` card carries the same CIS.

**On hardware** (2026-11-06, a GlazeBoot RAM reload over USB, indoors):

- The kernel locked onto NMEA at 4800 baud straight away, with no SiRF switch.
- Maps, installed with `glaze-usb put` and `run`, showed the receiver
  streaming, no fix and 1 satellite in view (PRN 14, not tracked).
- Before its first fix the receiver's RMC carries its own clock, which read
  2002-01-27.

A second reload gave up the search instead of locking. The cause was the
rate switch: switching the UART mid-character gives the right rate a framing
error, and the search treated any error as a wrong rate. It now re-arms the
UART up to four times per window, keeping DTR and RTS up, and makes six passes
before giving up. After that it locked at 4800 on the first pass on every
reload.

To try Maps on a GlazeBoot RAM boot, size the RAM disk for the pack. The
flash translation layer keeps a fifth of the disk in reserve, so a 3.4 MiB pack
needed `ramfs_kb=6656` in the artifact directory's `glaze.cfg`. A USB reload
stages the kernel and romfs in the RAM that's left, so build with
`GB_APPS="files settings"` (a 3.4 MiB romfs), or a large disk refuses the
next reload with `-2` (out of memory).

A fix taken outdoors hasn't been checked yet.

## Kernel

`kernel/drivers/cf.c` `capture_card()` checks, in order:

1. The exact SMART Bluetooth card.
2. Modems (a serial FUNCE naming a modem function).
3. Any other FUNCID-2 card with a COR and a CFTABLE entry decoding 3f8h. It
   becomes `BT_CARD_GPS` and goes to `kernel/gps.c`.

`gps.c` loads `drivers/cfuart.ko`. It writes the COR (entry index, level
interrupts) and sets 8N1 with DTR, RTS and OUT2.

**Baud search.** It listens at 4800, 9600, 38400, 19200 and 57600 baud, about
2.5 s each. It locks onto the first rate that delivers two NMEA sentences with
valid checksums. A wrong rate shows up as framing errors or silence.

**What is sent to the card.** Nothing, unless the receiver is seen framing
SiRF binary (`A0 A2` … `B0 B3`, for example left at 38400 by other software).
Then message 129, "Switch To NMEA Protocol", asks for NMEA at 4800: GGA, GSA and
RMC every second and GSV every five. A card that shows neither NMEA nor SiRF
binary after three passes is released.

**Losing the stream.** Five quiet seconds after the lock, or a UART error,
starts the search again.

The kernel keeps the latest valid RMC, GGA and GSA, and the last complete GSV
cycle. `gz_gps_io(GPS_IO_SNAPSHOT)` copies them out with their ages and a
sequence count. Any process may read this, as often as it likes, without
consuming anything. `GPS_IO_WRITE` sends up to 128 bytes, such as a `$PSRF`
command, while a receiver is streaming.

## Library

`lib/gps/gps.c` is the static `libgps.a`. The SDK ships it, and apps link it
with `LDLIBS += -lgps`, so it adds nothing to romfs. It provides:

- `gps_parse` and `gps_read`: the snapshot into a `struct gps_fix`.
  Positions are in 1e-7 degrees, altitude in decimetres, speed in cm/s,
  plus HDOP, the satellites used and in view, and per-satellite elevation,
  azimuth and SNR.
- `gps_command`: adds the `$`, the checksum and CR LF.
- `gps_project`: Web Mercator with the world 2^32 across, so a zoom-z
  pixel is 2^(24-z) units. It uses cubic Hermite interpolation of
  `tools/mkmercator.py`'s quarter-degree table, within 0.7 px at zoom 18.
- `gps_distance_m` and `gps_mm_per_pixel`.

Everything is integer: there is no FPU, and nothing links soft-float.

## Tile packs

`tools/mkmaptiles.py` reads an MBTiles file, or a tile server with a user
agent, attribution and rate limit, for a bounding box and zoom range. It
writes one `.gzmap`:

- a header and a sorted index;
- each 256-pixel tile as an RGB565 palette (up to 256 colours) and a raw
  deflate stream of indices, which Maps inflates with puff;
- identical tiles stored once.

Without Pillow it decodes 8-bit and palette PNGs itself. The default budget is
4 MiB, since the factory volume has about 9 MiB free. Use a source whose terms
allow offline copies: tile.openstreetmap.org forbids bulk downloads.

To install a pack, copy it to `/Maps` on the device (`glaze-usb put x.gzmap
/Maps/x.gzmap`), or to `/Card/Maps` before swapping in the GPS card. Generated
packs belong under ignored `out/`.

Example:

```sh
tools/mkmaptiles.py --mbtiles city.mbtiles --bbox 37.70,-122.52,37.83,-122.35 \
    --zoom 10-16 --out out/maps/city.gzmap
```

## Emulator and tests

- **The emulated card.** be300's `--cf-gps FILE.nmea` is a FUNCID-2 card with
  the real card's CIS (COR at `0x400`) and the modem's 16550 in GPS mode.
  It feeds the file one second at a time (through each RMC), at 4800 baud only.
- **Tracks.** `tools/mknmea.py` writes synthetic tracks. `tests/gps/track.nmea`
  is a 60 m circle at 37.7955 N 122.3937 W, with a three-second cold start.
- **Native tests.** `make -C host gps-test` runs `kernel/gps.c` against a
  mock UART, covering:
  - the 4800 and 9600 locks, the SiRF switch and giving up;
  - checksums, silence and removal;
  - parsing, including void fixes, both hemispheres and malformed fields;
  - Mercator and distances, checked against doubles.
- **ktest.** Booted with `--cf-gps tests/gps/track.nmea`, ktest checks the
  card kind, 4800 baud, a 3D fix on the track, satellites, the date and the
  sequence. Without the card it prints `SKIP gps`.
- **Tile packs.** `python3 tests/maptiles_test.py [--keep DIR]` round-trips
  palette, RGB and RGBA PNG tiles through a pack exactly. With `--keep` it
  leaves `demo.gzmap`, a synthetic street grid around the track.
- **The app.** glaze-apps has `make -C tests maps-ui-test` (native, with
  screenshots) and `tests/run_maps.py` (the emulator, with the package, the
  demo pack and `--cf-gps`).
