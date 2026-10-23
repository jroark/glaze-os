# Sega emulators for Glaze

Master System (`id=mastersystem`) and Genesis (`id=genesis`, also known as Mega
Drive) are separate installable apps. They use TotalSMS and ClownMDEmu,
respectively, with integer graphics and sound paths for the BE-300.

## Build and install

```sh
make -j4 out/pkg/mastersystem.gzp out/pkg/genesis.gzp
tools/mkcfimg.sh out/sega-cf.img 16 16 out/pkg/mastersystem.gzp out/pkg/genesis.gzp
```

Use the MIPS cross toolchain (`./build.sh` builds the normal NAND and all
packages). Boot with the card image, open each package in Files, choose Install
on Flash or Install on Card, then launch its home-screen icon. A Card install
requires that card to remain inserted. Packages are separate from the OS romfs;
building the NAND does not preinstall them.

Each app includes an original CC0 Glaze demo: arrows move a diamond, the action
buttons produce sound, and its position is saved in cartridge RAM. The Genesis
demo also exercises FM audio and handled 68000 exceptions. Python generates
these cartridges under ignored `out/pkg-res/`; no ROM binary is tracked.
`tools/mksegademo.py sms` and `tools/mksegademo.py genesis` regenerate them using
the standard library. `tools/mksegaicons.py` regenerates the original icons
(Pillow required). Keep all other ROMs outside the tree or under ignored `out/`,
including `.md` files: that suffix also names Markdown, so Git cannot ignore it
globally. No commercial cartridge, BIOS or Sega CD boot ROM is included.

The library browses Documents and Card, including subdirectories and paged
listings. Master System accepts uncompressed `.sms`. Genesis accepts raw
big-endian `.gen`, `.md` and `.bin`. ZIP/GZIP archives, copier headers and
interleaved `.smd` images must be converted externally.

## Controls, display and audio

| BE-300 / emulator input | Master System | Genesis |
| --- | --- | --- |
| Physical arrows | D-pad | D-pad |
| OK / Esc | Button 1 / Button 2 | B / C by default |
| Enter (keyboard) | Console Pause | Start |
| Tab (keyboard) | Button 1 | Action not assigned to OK/Esc |
| Pause menu action | Console Pause | Start or the unassigned action, for one frame |
| Pause menu mapping | — | Cycle OK/Esc through B/C, A/B, A/C |
| Library | Save and return to ROM selection | Same |
| Home | Save, release audio and suspend | Same |

There are no on-screen gameplay controls. The navigation bar provides Library
and Pause; the pause menu also has Reset, Sound and NTSC/PAL selection. Changing
the TV standard resets the machine while retaining cartridge RAM. A hardware-only
Genesis setup has two simultaneous action buttons; the pause-menu action is a
momentary fallback, not a substitute for a third held button. Six-button pads and
multiplayer are not exposed. Tap the home-screen icon to resume a suspended app.

The entire picture is centered below navigation and scaled to 240 pixels wide
with nearest-neighbor sampling, preserving the framebuffer aspect ratio.
Master System renders 256x192/224/240; Genesis renders 256/320x224/240. PAL/NTSC
targets are 50/60 frames per second. Every other frame is presented; CPU, input
and audio run every emulated frame. Streams use 22050 Hz mono, with bounded
buffering. Master System has an integer SN76489 implementation; Genesis mixes
the core's YM2612 FM and PSG outputs using integer downsampling. Audio and video
timing are approximate; slow emulation can underrun audio. Neither performance
nor compatibility has been established on physical hardware.

## Cartridge support and saves

Master System accepts power-of-two images from 8 KiB through 4 MiB, with a
Master System header or a recognized SMS CRC in the core database. Sega and
Codemasters bank switching are tested; other database-selected mapper variants
remain unverified. Game Gear, SG-1000 and YM2413 FM expansion are not exposed.
The app stores the core's 32 KiB cartridge RAM window because SMS headers do not
identify battery RAM. A BIOS is not required.

Genesis accepts even-sized raw images from 512 bytes through 4 MiB with a SEGA
header and valid reset vectors. Standard cartridge SRAM at 0x200000/0x200001 is
supported up to a 64 KiB address window. EEPROM and other declared save hardware
are rejected. Special cartridge coprocessors, large-ROM mapper extensions,
Sega CD, 32X and interlaced output beyond 240 lines are not supported. Reset
vectors and save metadata are validated, but this is not an exhaustive game
compatibility guarantee. The app selects overseas NTSC unless the header
specifies Europe without Japan/USA; the pause menu can override TV timing.

Saves live at `<bundle>/data/<ROM-content-fingerprint>.sav`. They follow content
across filename changes. Changed RAM flushes every five seconds, on pause,
reset, TV changes, Library and Home. Reset retains RAM. Writes use a temporary
file and rename, including recovery from interrupted FAT replacement. An
existing save that cannot be restored blocks loading; write errors appear in
the app. Power loss can lose recent unflushed changes. Uninstalling removes
the app's saves. There are no save states or rewind.

Large images need free RAM: Genesis makes a word-swapped copy while the frontend
retains the input buffer; Master System borrows the input. Allocation failures
return to the library. Suspended apps reduce available memory.

## Validation

```sh
make -C host mastersystem-test sms-banks-test genesis-test
python3 tests/sega_smoke.py --be300 /path/to/be300
```

Native tests use ASan/UBSan. Both adapters execute original generated cartridges
and check changing graphics, actual controller reads, nonzero PCM, RAM retention
across reset/TV changes, PAL execution, rejection of malformed input and
preservation of source ROM bytes. Genesis additionally executes divide-by-zero
and ILLEGAL/RTE exception paths. SMS mapper tests check fixed/banked ROM, Sega
and Codemasters registers, RAM mirrors, SRAM banks, word accesses crossing bus
boundaries, 8 KiB ROM mirrors and 224/240-line framebuffer bounds.

The headless smoke runner installs each app on disposable NAND/CF copies,
loads both its demo and a Card copy, and exercises controls, audio stream paths,
pause/reset, save restoration, TV switching, button mapping and Home/resume. It
checks guest logs for faults and restores a normal unscripted NAND build in
`finally`. Logs and screenshots are under `out/shots/sega`. Use the cross-toolchain
environment for this runner. Boot disposable images: the BE-300 emulator writes
NAND and CF back on exit. These tests do not establish cycle accuracy or broad
commercial-game compatibility.

See [TotalSMS provenance](../third_party/totalsms/UPSTREAM.md) and
[ClownMDEmu provenance](../third_party/clownmdemu/UPSTREAM.md) for upstream pins,
licenses and local changes.
