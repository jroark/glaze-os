# NES for Glaze

NES is an installable app (`packages/nes`, id `nes`). Build it with the normal
MIPS toolchain:

```sh
make -j4 out/pkg/nes.gzp
```

Put `out/pkg/nes.gzp` on a CompactFlash image, open it in Files, and choose
**Install on Flash** or **Install on Card**. Launch NES from the home screen.
The package includes the InfoNES license and an original, public-domain demo;
no commercial ROMs are included. Assets and the app add about 130 KiB to the
installation, without consuming the OS romfs slot.

The library opens `.nes` files in Documents or Card and their subdirectories.
Directory listings are paged and scrollable. Open **Glaze demo** to check the
emulator without supplying a ROM: move the sprite, hold A to change its colour
and play a pulse tone, and press Start to recentre it.

## Controls and display

| Control | NES action |
| --- | --- |
| Physical arrows | D-pad |
| OK / Esc | A / B |
| Enter / Tab (keyboard) | Start / Select |
| Start / Select in pause menu | Send the button once and resume |
| Pause | Show Reset, Sound On/Off, Start and Select |
| Library | Save battery RAM and return to ROM selection |

Gameplay uses the BE-300 physical buttons or a keyboard. There is no on-screen
controller or touch input over the game picture. Start and Select remain
available from the pause menu when a keyboard is not attached. Home sends the
app to the background through SpringBoard, saving battery RAM and releasing
the audio stream. Tapping its icon resumes the cartridge.

The full 256x240 image is scaled to 240x225 and centred in the area below the
navigation bar, preserving its aspect ratio. The system status bar remains
visible. The emulator targets 60
NTSC frames per second with every other video frame rendered. CPU/APU/input
still advance on unrendered frames. PCM is integer mono at 22050 Hz, sent to
the existing audio server, with bounded buffering. Timing and sound are
approximate; audio can underrun when emulation is slower than real time.

## Cartridge support and saves

Supported: standard NTSC iNES cartridges, mappers 0 (NROM), 1 (MMC1), 2
(UxROM), 3 (CNROM), and 4 (MMC3), CHR ROM/RAM, trainers, and up to 8 KiB of
battery SRAM. The loader validates bank sizes and complete payload lengths
before exposing the image to the core. Limits are 512 KiB PRG, 256 KiB CHR,
1 MiB total file, power-of-two bank counts, and mapper-specific restrictions.
NES 2.0, PAL, Vs./PlayChoice, other mappers and larger save RAM are rejected
with an explanation. This scanline core does not promise compatibility with
all games; PPU/APU timing and some mapper variants are approximate.

Battery saves are keyed by the ROM content fingerprint in
`<app bundle>/data/<fingerprint>.sav`, not beside the ROM. They are flushed
when changed every five seconds, on pause, reset, Home, and return to the library.
A completed temporary file is renamed into place; interrupted FAT replacement
can recover from the temporary file. Reset preserves SRAM. Power loss or a
crash can lose the most recent five seconds. Uninstalling the app removes
its saves; installing on Card requires that card to remain inserted. Save
states, rewinding, PAL and two-player support are not implemented.

## Validation

The portable test builds with AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
make -C host nes-test
```

It checks malformed/truncated headers, mapper 0–4 bank selection, RGB555
conversion and 120 frames of the original demo. It checks actual
6502 controller reads into SRAM, moving PPU output, nonzero APU samples and
SRAM retention across reset. Regenerate the demo with `python3 tools/mknesdemo.py`.

An optional external CPU oracle uses Kevin Horton's `nestest.nes` and Nintendulator
reference `nestest.log`, available in the `other/` directory of
https://github.com/christopherpow/nes-test-roms. These files are not vendored.

```sh
make -C host nes-cpu-test NES_TEST_ROM=/absolute/path/nestest.nes \
    NES_TEST_LOG=/absolute/path/nestest.log
```

All 8,991 instruction states and CPU cycle counts are checked. The test ignores
status bit B when comparing registers, because B is a stack encoding rather
than a physical stored flag. Stable unofficial composite instructions are
covered; this does not establish PPU timing accuracy.

`tests/t_nes.txt` exercises installation, the demo, controls, pause/resume,
loading the same cartridge from Card, save restoration, reset, and Home/warm resume. Build with
`make -j4 INPUT_SCRIPT=tests/t_nes.txt`, then boot a copy of the resulting NAND
with a card containing `nes.gzp` and the demo renamed `test.nes`. Run for 46
seconds, capture both serial streams, and check `nes: loaded`, `nes: paused`,
`nes: resumed`, `nes: battery restored` and `nes: reset`. Always boot copies:
the BE-300 emulator writes NAND and CF on exit. Do not leave scripted input in
a production image; finish with a normal `make -j4`.

The verified BE-300 emulator run measured about 56 frames per guest second
with the demo. That is an emulator measurement, not validation on physical hardware
or a performance guarantee for games. See `third_party/infones/UPSTREAM.md`
for core provenance and portability/correctness changes.
