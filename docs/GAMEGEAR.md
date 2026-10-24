# Game Gear for Glaze

Game Gear is a separate installable app (`id=gamegear`) using the same pinned
TotalSMS core as Master System. It exposes Game Gear hardware mode, its cropped
LCD viewport, 12-bit palette and polled Start button.

## Build and install

```sh
make -j4 out/pkg/gamegear.gzp
tools/mkcfimg.sh out/gamegear-cf.img 16 16 out/pkg/gamegear.gzp
```

Use the MIPS cross toolchain; `./build.sh` builds the normal NAND and all
packages. Open the package in Files, install on Flash or Card, then launch its
home-screen icon. A Card install needs that card present. The package is not
preinstalled in the factory NAND.

The library offers an original CC0 Glaze demo and browses Documents/Card for
uncompressed `.gg` files, including subdirectories and paged listings. The demo
has striped tiles, a diamond controlled by the arrows, button-driven PSG audio,
Start readback and persistent position. `tools/mksegademo.py gamegear` generates
it under ignored `out/pkg-res/gamegear/`. All ROM binaries, including generated
demos and uploaded games, stay out of Git. No commercial cartridge or boot ROM
is included. `tools/mksegaicons.py` regenerates the original icon (Pillow needed).

## Controls and presentation

| Input | Action |
| --- | --- |
| BE-300 arrows | D-pad |
| OK / Esc | Button 1 / Button 2 |
| Enter (keyboard), or Start in Pause | Start |
| Pause | Stop emulation; Play, Reset, Sound and Start |
| Library | Save and return to ROM selection |
| Home | Save, release audio and suspend; tap the icon to resume |

There are no on-screen gameplay controls. The entire 160x144 LCD picture is
cropped from the core's 256x192 canvas and enlarged to 240x216 using nearest
neighbors. Start reads through port 0 rather than triggering the Master System
console's NMI. Game Gear always uses NTSC timing; there is no TV toggle.

The integer SN76489 backend applies Game Gear's stereo routing mask before
downmixing both channels to the app's 22050 Hz mono stream. Volume and tone/noise
timing are approximate and output is not bandlimited. Emulation targets nominal
60 Hz; every other frame is presented while CPU/input/audio advance each frame.
The Glaze emulator demo run was below real time (about 48 fps including UI
interruptions); physical BE-300 performance remains unverified.

## Cartridge support and persistence

Images must be power-of-two sizes from 8 KiB through 4 MiB, with a Game Gear
header (Japan, Export or International), or a recognized GG CRC in TotalSMS's
database. SMS and SG-1000 cartridges are rejected. The app uses export mode;
region-specific behavior beyond the available core support is unverified. ZIP,
copier headers, link cable and other peripherals are not supported.

The core supports Sega and database-selected cartridge mapper variants. The
shared Sega mapper boundaries are covered by Master System tests; compatibility
with every GG game/mapper is not established. No BIOS is needed.

The 32 KiB cartridge RAM window is saved to
`<bundle>/data/<ROM-content-fingerprint>.sav`. Changes flush every five seconds,
on Pause, Reset, Library and Home. Reset preserves RAM. Temporary-file/rename
replacement supports interrupted FAT recovery. An unreadable existing save
blocks loading; write failures appear in the app. Power loss can lose recent
unflushed changes, and uninstalling deletes the app's saves. Save states and
rewind are absent. The ROM is borrowed by the core and remains unchanged.

## Validation

```sh
make -C host gamegear-test gamegear-hardware-test
python3 tests/sega_smoke.py --be300 /path/to/be300 --system gamegear
```

Native ASan/UBSan tests run the original cartridge and verify changing pictures,
real pad/Start reads, varying PCM, source preservation and reset retention. The
hardware test checks 12-bit palette values, framebuffer bounds, Start without
NMI and left-only/right-only/muted PSG routing. Sanitizer errors are fatal.

The Glaze smoke test installs from a disposable CF image, loads demo and Card
cartridges, exercises input, sound, pause/reset, save restoration and Home/resume,
checks logs for faults and restores the normal unscripted NAND in `finally`.
Outputs are under `out/shots/sega/gamegear`. Use the cross-toolchain environment;
the emulator modifies NAND/CF images, so boot disposable copies. `--gg-rom`
accepts an external test ROM; keep it external or under ignored `out/`.

Provenance and shared port changes: [TotalSMS](../third_party/totalsms/UPSTREAM.md).
