# TurboGrafx-16 / PC Engine for Glaze

TurboGrafx-16 is an installable HuCard app (`id=turbografx16`) using the C
HuC6280 CPU, VDC/VCE and PSG from Beetle PCE Fast. Glaze supplies cartridge bus
mapping, controls, integer PCM reconstruction, a ROM library and save handling.
The package emulates the base NTSC PC Engine/TurboGrafx-16.

## Build and install

```sh
make -j4 out/pkg/turbografx16.gzp
tools/mkcfimg.sh out/pce-cf.img 16 16 out/pkg/turbografx16.gzp
```

Build with the MIPS cross toolchain (`./build.sh` builds the normal NAND and all
packages). Open the package in Files, install on Flash or Card, then launch the
home-screen icon. Card installs require the card to remain inserted. The app
is separate from the OS romfs and is not preinstalled in the factory volume.

The original CC0 Glaze HuCard demo has striped background tiles, a diamond
sprite, pad reads, wave PSG audio and persistent position. Arrows move it;
I/II enable its tone. `tools/mkpcedemo.py` generates the cartridge under ignored
`out/pkg-res/turbografx16/`. No ROM binary is tracked, including original demos
and uploaded games. No BIOS, commercial cartridge or CD content is included.

Browse Documents or Card, including subdirectories and paged listings, for
uncompressed `.pce` files. The app accepts native-order HuCards, optionally with
a 512-byte copier header. It strips that header for mapping and save identity.
ZIP files and bit-reversed dumps must be converted externally.

## Controls, display and audio

| Input | Action |
| --- | --- |
| BE-300 arrows | D-pad |
| OK / Esc | I / II |
| Enter / Tab (keyboard) | Run / Select |
| Run / Select in Pause | Send one frame of input and resume |
| Pause | Play, Reset, Sound, Run and Select |
| Library | Save and return to cartridge selection |
| Home | Save, release audio and suspend; tap the icon to resume |

There are no on-screen gameplay controls. The controller uses a single standard
two-button pad; six-button controllers, multitap and mice are not exposed.

The core renders RGB565 with a fixed 240-line visible window and 256-, 352- or
512-pixel widths selected by the VCE dot clock. The whole window is scaled to
240x180 for a 4:3 television picture. Mid-frame width changes are approximate.
There is no PAL toggle. The frontend targets nominal 60 Hz and presents every
other frame, with CPU/input/audio advancing on every frame; exact 262/263-line
timing remains in the core.

PSG logic retains six wavetable channels, DDA, noise and LFO. Glaze replaces
upstream Blip filtering with integer timestamped delta accumulation and step
reconstruction at 22050 Hz, then downmixes stereo balance to mono. Audio is
approximate and not bandlimited; this is not a claim of sound/cycle accuracy.
Slow emulation can underrun audio. Physical BE-300 performance is unverified.
The original demo measured about 31 frames per guest second in the BE-300
emulator, including UI interruptions, below its nominal 60 Hz target.

## Cartridge support and saves

Supported image sizes are powers of two from 8 KiB to 1 MiB, plus 384 and
768 KiB. The mapper implements small-ROM mirrors, the 384/512 KiB split-ROM
layouts, and an absent upper 256 KiB window for 768 KiB images. The core's
8 KiB work RAM is mirrored at physical banks F8–FB. A Populous signature
selects its 32 KiB cartridge RAM; other cartridges expose 2 KiB backup RAM at
bank F7. The initial backup RAM contains the standard formatted header.

Malformed sizes and empty/invalid reset vectors are rejected, but HuCards have
no mandatory identifying header, so validation cannot prove that arbitrary data
is a game. Street Fighter II's 2.5 MiB mapper, SuperGrafx, CD-ROM/System Cards,
Arcade Card and special peripherals are not supported. SuperGrafx images have
no reliable universal header and must not be used with this base-system app.
No BIOS is required for HuCards.

Saves live at `<bundle>/data/<ROM-content-fingerprint>.sav`. A copier header
does not change the fingerprint. Changed RAM flushes every five seconds and
on Pause, Reset, Library and Home. Reset retains cartridge/backup RAM. Saves
use temporary-file/rename replacement with FAT recovery; unreadable existing
saves block loading, and write errors appear in the app. Recent changes can
be lost on power failure, and uninstalling removes saves. Save states and
rewind are absent.

The adapter copies ROM into its cartridge map while the frontend retains the
source buffer. ROM bytes remain unchanged. Allocation/read errors return to the
library; large images and other suspended apps reduce available RAM.

## Validation

```sh
make -C host turbografx16-test pce-hardware-test
python3 tests/sega_smoke.py --be300 /path/to/be300 --system turbografx16
```

ASan/UBSan tests execute the original HuCard and check changing graphics,
CPU-facing pad/Run/Select reads, varying PCM, copier-header normalization,
ROM preservation and reset retention. Hardware tests check ROM bank boundaries,
384/512 KiB layouts, work RAM mirrors, backup RAM bounds, Populous RAM retention,
all six wave channels, both noise channels, DDA and invalid PSG selectors.
Sanitizer errors are fatal. The LFO signed-shift fix is covered by the register
edge cases; detailed LFO sound accuracy has not been established.

The Glaze smoke runner installs the package on disposable NAND/CF copies,
plays the demo and a Card copy, and tests controls, Run/Select, sound,
pause/reset, save restoration and Home/resume. It checks logs for faults and
restores the normal unscripted NAND in `finally`. Output is under
`out/shots/sega/turbografx16`. Use the cross-toolchain environment; boot disposable
images because the BE-300 emulator modifies NAND and CF on exit. `--pce-rom`
accepts an external HuCard for Card loading; keep it outside Git.

The generated demo tests do not establish commercial-game compatibility. Core
pin, licensing and port changes: [Beetle PCE Fast](../third_party/beetle-pce-fast/UPSTREAM.md).
