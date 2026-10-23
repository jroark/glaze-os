# Game Boy for Glaze

Game Boy is an installable app (`id=gameboy`) for original Game Boy (DMG) and
Game Boy Color (CGB) cartridges. It is separate from the NES app and uses a
portable gnuboy core with integer graphics/audio.

## Build and install

```sh
make -j4 out/pkg/gameboy.gzp
```

Use the existing `mips-cross-dev` Docker toolchain (`./build.sh` builds the normal
NAND and all packages). Copy `out/pkg/gameboy.gzp` onto a CompactFlash image,
open it in Files, choose Install on Flash or Install on Card, and launch Game Boy
from the home screen. Installation on Card requires that card to remain present.
The app adds no program or ROM to the OS romfs slot.

The library includes original CC0 **Glaze DMG demo** and **Glaze Color demo**
cartridges. Arrows move the diamond; A/B play a tone; Start recentres it. The
horizontal position persists in battery RAM. The Color demo also exercises
CGB double speed. Builds and native tests generate demo ROMs in ignored `out/pkg-res/gameboy/`;
no ROM binary is tracked. `python3 tools/mkgbdemo.py` regenerates those outputs
using only Python’s standard library. To also regenerate the original icon, add
`--icon packages/gameboy/icon.gzi` (Pillow required). Keep user-provided ROMs
external or under `out/`. No Nintendo boot ROM, logo or
commercial game content is bundled.

Browse Documents or Card and their subdirectories for uncompressed `.gb` and
`.gbc` files. Listings are paged and scrollable. Hardware mode comes from the
cartridge header, rather than the filename. Supported dual-mode games run as CGB.

## Controls, display and sound

| BE-300 / emulator input | Game Boy action |
| --- | --- |
| Physical arrows | D-pad |
| OK / Esc | A / B |
| Enter / Tab (keyboard) | Start / Select |
| Start / Select in pause menu | Send one frame of input and resume |
| Pause | Resume, Reset, Sound On/Off, Start and Select |
| Library | Save and return to ROM selection |
| Home | Save, release audio and suspend; tap the app to resume |

There are no on-screen gameplay controls. The entire 160x144 picture is enlarged
to 240x216 with nearest-neighbor sampling and centered below navigation. DMG
uses grayscale and CGB uses cartridge palettes, converted to RGB565.

Emulation targets 70,224 clocks per frame at 4,194,304 Hz (about 59.73 Hz).
Every other frame is presented; emulation, input and audio advance every frame.
CGB double speed changes CPU timing, not the screen rate. Sound is 22050 Hz mono,
with bounded buffering through Glaze's audio server. Audio may underrun when
emulation is slower than real time. Guest logs report frames and elapsed time;
the verified BE-300 emulator run measured about 42–43 frames per guest
second for the Color demo. This is below the target rate and does not establish
physical-hardware performance.

## Cartridges and persistence

Supported controllers: ROM-only (including RAM variants), MBC1, MBC2, MBC3 with
optional RTC, and MBC5 with or without rumble. Rumble cartridges play without
vibration. Limits are 8 MiB ROM and 128 KiB RAM, further constrained by controller
addressing. MBC2 uses 512 four-bit RAM cells. MBC1M multicarts and exotic controller
variants are not supported. ZIP/GZIP, special ROM-size codes 0x52-0x54, unsupported
controllers, incomplete files and inconsistent RAM declarations are rejected.
Header-checksum mismatches warn without blocking otherwise valid homebrew.
A boot ROM is not required: the adapter initializes post-boot state directly.

ROM bytes are borrowed by the core and never modified or copied into a second
large buffer. A failed allocation/read returns to the library with an error. A battery save
that exists but cannot be fully restored also blocks loading, preserving that save.
Large ROMs require enough free RAM; other suspended apps can reduce availability.

Battery RAM is stored in `<bundle>/data/<ROM-content-fingerprint>.sav`, with
versioned, checksummed MBC3 clock metadata in the adjacent `.sav.rtc` file. Saves
follow ROM content across renames or copies. Changes flush every five seconds,
on pause, reset, Library and Home. Write failures appear in the game or pause
screen and in the library after returning. A completed temporary file is renamed into
place; interrupted FAT destination replacement can recover from that temporary
file. Recent unsaved changes can be lost on power failure. Uninstalling the app
removes its saves. Reset preserves battery RAM and RTC.

The MBC3 clock advances from wall time during play, pause, suspension and between
launches, respects halt/latch, and wraps after 512 days with sticky carry. Moving
the device clock backward contributes zero elapsed time for that change. An
unavailable RTC retains the previous valid reference. Save states, rewind, link
cable, Super Game Boy enhancements, infrared and special peripherals are absent.

## Validation

```sh
make -C host gameboy-test
make -C host gameboy-rom-test GB_TEST_ROM=/path/to/cpu_instrs.gb
out/host/gameboy-rom /path/to/instr_timing.gb
python3 tests/gameboy_smoke.py --be300 /path/to/be300
```

Native tests use ASan/UBSan and cover cartridge rejection, all supported controller
families, maximum ROM/RAM banks, DMG/CGB demos, controls, sound, source-byte
preservation, SRAM reset retention, clock metadata and rollover, CGB memory/DMA,
window clipping, partial writes and interrupted FAT save replacement.

The optional ROM runner checks serial results from Blargg's `cpu_instrs` and
`instr_timing`, available at https://github.com/retrio/gb-test-roms. These ROMs
are external and not vendored. Graphics and sound retain gnuboy's approximate
timing; these tests do not establish pixel/cycle accuracy for every game.

The headless smoke runner builds scripted input, installs the package on a copy
of the NAND, exercises both demos and Card loading, controls, Start/Select, sound,
pause/reset, battery/clock restoration and Home/warm resume. It checks guest
logs for faults and restores a normal unscripted NAND build in `finally`.
Boot disposable NAND and CF images: the BE-300 emulator writes them on exit.
Logs and screenshots are under `out/shots/gameboy`.

Core provenance and portability fixes: `third_party/gnuboy/UPSTREAM.md`.
