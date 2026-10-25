# SomaFM radio

SomaFM is an installable, unofficial player for personal listening. It needs
the NE2000 Ethernet card (`--ne2000` in the emulator) and this version of
Glaze's audio service. Start the emulator with `--audio` to hear playback.
An older OS can load the package and explains that an OS update is needed.

Build with `./build.sh`. Install `out/pkg/somafm.gzp` through Files or include
it on a CompactFlash image with `tools/mkcfimg.sh`. The App Store catalog
includes the app; normal store build/publishing procedures apply. Nothing is
published by building this package.

Tap a station to listen. Now Playing has Play/Stop (Retry after an error),
current-track metadata when supplied by the station, and Add/Remove Favorite.
All Stations and Favorites filter the list. Up/Down selects a station; OK
plays it. In Now Playing, OK toggles playback and Esc returns to the list.
Left/Right switches the station-list filter. Favorites and the last selection
are stored in the package's `data/radio` file. Opening the app does not start
playback automatically. Pressing Home leaves playback running; Music and
Videos take over when they start playing.

## Stations and connectivity

The checked-in catalog has 51 stations from SomaFM's public
[listen page](https://somafm.com/listen/), using official HTTPS MP3 PLS links.
128 kbps is preferred, otherwise the lowest published MP3 bitrate is selected.
The catalog includes seasonal and event channels, which may be offline.
Builds do not contact SomaFM. To refresh the catalog intentionally:

```sh
python3 tools/update_somafm.py
# Or supply a previously downloaded public listen page:
python3 tools/update_somafm.py /tmp/somafm-listen.html
```

The [catalog API is unavailable to third parties](https://somafm.com/linktous/api.html).
The app uses neither that API nor runtime page scraping. Each tune resolves
the station's PLS playlist and tries its listed servers. TLS verifies the
certificate, hostname, and device date against the OS's bundled roots; an
HTTPS redirect cannot downgrade to HTTP. No account or companion server is
required. SomaFM's broadcasts are listener supported: [support SomaFM](https://somafm.com/support/).

## Playback implementation

The existing audio service owns playback independently of the app. One lazy,
persistent network worker resolves playlists and receives HTTP/ICY data into
a 128 KiB compressed-audio ring. The audio loop incrementally decodes MP3,
resamples it, and feeds the existing PCM output, keeping IPC and system sounds
responsive. Startup and rebuffering require 24 KiB of compressed audio. No
broadcast data is written to flash.

HTTP parsing supports fragmented headers and bodies, chunked transfer,
unknown-length live streams, five redirects, and inline ICY track metadata.
Playlists are capped at 32 KiB and 16 distinct stream addresses. Transient
disconnects try alternate servers and then retry the playlist five times,
after 1, 2, 4, 8, and 15 seconds. Certificate, format, memory, and missing-card
errors stop automatic retries. Stop and audio takeover cancel pending work;
DNS is bounded to two seconds and other socket operations are nonblocking.

`glaze/radio.h` defines optional audio-server requests `AUD_RADIO_PLAY`,
`AUD_RADIO_STATE`, and `AUD_RADIO_STOP`, starting at operation `0x50`.
Requests and replies use the existing `AUD_HELLO` shared-memory session.
Existing audio operation numbers and `audio_state` are unchanged. The package
uses existing IPC syscalls instead of new libc exports, so unsupported older
services return `GZ_EINVAL` and the package remains loadable.

Browser, App Store, and audio share `libwolfssl.so.1` to fit the fixed ROMFS
space. Their TLS context and transport state remain private to each process.
The original icon is reproducible with `python3 tools/mksomafm_icon.py`.

## Verification

```sh
make -C host radio-test
make -C host radio-worker-test
make -C host somafm-ui-test
make -C host store-http-test
./build.sh INPUT_SCRIPT=tests/t_somafm.txt
python3 tests/run_somafm.py --seconds 50 --expect-playing
```

Native tests exercise fragmented HTTP/ICY and MP3 input, malformed responses,
redirects, alternate servers, full-buffer backpressure, cancellation, TLS
rejection, favorites persistence, input handling, and older-service UI.
They write UI previews under `out/host/somafm-*.bmp`.

To exercise package installation through Files, build with
`INPUT_SCRIPT=tests/t_somafm_install.txt` and run
`python3 tests/run_somafm.py --seconds 50 --install-from-files --expect-playing`.
This starts without SomaFM installed, opens its package in Documents, installs
it on flash, launches it, and verifies live playback after Home and return.

Real audio-service handoff checks run without SpringBoard as an optional
guest fixture, using live SomaFM by default:

```sh
./build.sh RADIO_TESTS=1 INIT=bin/radio_guest
python3 tests/run_somafm.py --seconds 60 --guest
# Restore a normal OS build afterwards:
./build.sh
```

For the background soak test, build with
`INPUT_SCRIPT=tests/t_somafm_soak.txt`, then run
`python3 tests/run_somafm.py --seconds 660 --expect-playing`. The script opens
Calculator while SomaFM plays, then returns to SomaFM after ten minutes.
`BE300` can override the emulator executable. Logs, screenshots, and disposable
NAND images stay in the printed `out/somafm-*` directory. `--no-net` exercises
the missing-card screen. Never run the emulator against the build's original
NAND image when a disposable copy will do.
