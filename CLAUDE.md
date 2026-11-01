# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Glaze is a microkernel OS for the Casio BE-300 (NEC VR4131 MIPS, 240x320 RGB565 LCD, 16 MiB SDRAM, 16 MiB small-page NAND) with an early-iPhone-OS-style UI. It boots through the stock boot ROM from a NAND image on the [be300 emulator](https://github.com/jroark/be300).

## Building

```sh
./build.sh                        # Docker (mips-cross-dev image) -> out/glaze.bin
make -j8                          # same, natively, if mipsel-linux-gnu-gcc is installed
make -j8 INIT=bin/ktest           # boot the kernel self-test instead of SpringBoard
make -j8 INPUT_SCRIPT=tests/ui/x.txt # replay scripted input (see Testing)
make -C host fstest               # native build of the file-system stack test
make -C host fat-test             # CF/FAT volume test on FAT12/16/32 images, then fsck.fat
make -C host ui-events-test       # libui's touch cancel, suspension and resume, natively
make glazeboot                    # GlazeBoot's GCC parts, booting Glaze from RAM out of Windows CE (docs/dev/GLAZEBOOT.md)
make glazeboot-exe                # the GlazeBoot.exe loader, natively with ../be300-dev (BE300_DEV)
make -C host glazeboot-test       # its page hand-over plan on random layouts
make -C host nand-ecc-test        # Windows CE's NAND ECC (kernel/drivers/nand_ecc.c), natively
tests/run_ramboot.sh 90 INIT=bin/ktest  # RAM boot through GlazeBoot's trampoline in the emulator
tests/run_ceboot.sh 90            # GlazeBoot from a CF card under the emulator's stock Windows CE
```

There is no linter; the build uses `-Wall -Wextra` and should stay warning-free. `compile_flags.txt` is for clangd.

`make sdk-archive` exports the standalone C application SDK to `out/sdk/` without
building a NAND. `make sdk-test` tests relocated, out-of-tree app builds,
package validation and included library source rebuilds. SDK build rules,
templates and CLI live in `sdk/`; the exporter is `tools/mksdk.py`.
See `docs/dev/SDK.md`. Keep packaged SDKs and generated apps under ignored output.

`main.o`, `input.o` and `romfs.img` are always rebuilt because `INIT`, `BUILD_ID` and `INPUT_SCRIPT` are baked into them. Every directory under `apps/` and `servers/` becomes a program, `bin/<dirname>` in romfs, with no Makefile edit, except the optional `ldtest*` and `ldmem` test apps, and `ktest`/`ktest_pong`, which go in only when `INIT=bin/ktest`. romfs is nearly full (its slot ends at 0x500000), which is why wolfSSL is built `-Os`; `NAND_DROP=<apps>` leaves apps out of a NAND image's romfs (`NAND_DROP=wince` makes room for `WL54CF=1`'s firmware, and for `INIT=bin/ktest`). Assets in `assets/gen/` are checked in; they are regenerated only on macOS with `swift tools/mkassets.swift assets/gen assets/fonts userdata/Pictures`. The system sounds in `assets/sounds/` (romfs `sounds/`) are checked in too, synthesized by `tools/mksounds.py assets/sounds`.

Romfs is for the OS and built-in apps only. User files, settings and app data belong in the file system: `userdata/` is the factory volume, which `mkfsimg` (`out/buildtools/mkfsimg`, built with `HOSTCC`, default `cc`; `make -C host mkfsimg` builds a native copy in `out/host`) formats into `out/fs.img` and `tools/mk_nand.py --fs-image` places at 0x500000. Don't name files in `userdata/` with spaces, since make lists them.

ROM binaries must not be committed, including original demos and uploaded test games.
Keep external ROMs outside the source tree or under ignored `out/`. The console
emulators and their generated demo cartridges live in the sibling glaze-emulators repo.

Never commit secrets or vendor material: the OTA signing key (`~/.config/glaze/ota-ed25519.pem`),
`DEBUG.KEY` and other pairing files, `CODEX.AUTH` login exports and other credentials,
Doom WADs, or binaries, dictionaries, maps and NAND images recovered from Windows CE.

## Running and testing

```sh
be300 --nand out/glaze.bin --rtc-host-time > stdout.log 2> stderr.log
be300 --nand out/glaze.bin --rtc-host-time --ne2000   # with the PCMCIA Ethernet card
tools/mkcfimg.sh out/cf.img [size_mb [12|16|32]] [files...]  # FAT image (dosfstools, mtools)
be300 --nand out/glaze.bin --rtc-host-time --cf out/cf.img    # with a CompactFlash card
```

- The emulator writes NAND back into the image, so boot a copy when you need a clean flash. Kernel and `printf` output goes to the emulator's stdout. A screenshot (`screenshot_*.bmp`) is saved on orderly shutdown, including SIGTERM from `timeout`. With no display, `SDL_VIDEODRIVER=dummy` runs it headless and still saves the screenshot.
- **ktest** (`apps/ktest`): build with `INIT=bin/ktest`, boot, and grep stdout for `ktest: FAIL` / `ktest: ALL PASS`. It covers IPC, shm (including unmapping), sleep, faults, threads and semaphores, task control (background, suspension, child-exit notification), the file server and preferences, packages and spawning from memory (packages need `--cf` with `out/pkg/hello.gzp` on the card), the audio server (stream clock and underruns, MP3 play/seek/pause, ADPCM WAV), time zone rules, and with `--ne2000` the network: DHCP, ping, DNS through the NAT, TCP/UDP over lwIP's loopback through both `glaze/net.h` and BSD sockets. Boot it both with and without `--ne2000`; without a card the network cases print `SKIP`. With `--cf <image>` it also tests `/Card` (and leaves `KTEST.TXT` on it); without a card those cases print `SKIP`. It prints `INFO` for an internet fetch and an NTP query it can't require.
- **Scripted UI tests**: `tests/ui/*.txt` replay timed events (`<ms> tap x y`, `drag ...`, `key ok|esc|enter|...|home`, `text <characters>`) from inside the kernel, in screen coordinates. `tests/run.sh tests/t_x.txt` builds, boots (with `CF=<image>`, a copy of that card) and converts the final screenshot, but it uses the macOS tools `gtimeout` and `sips`. Scripted events bypass the touch panel and its interrupts.
- **Real touch path**: `tests/touch.sh tests/ui/touch_unlock.act` drives the emulator window with xdotool on an X display (`Xvfb :99 &`, then `DISPLAY=:99`). Action coordinates are window/screen pixels; app surfaces start 16 px lower, under the status bar.
- **Audio**: boot with `--audio` to hear it and `--log-mmio` for the emulator's `[AIU] start`/`stop` lines. Its host output is resampled and drops samples under load, so check exactness against the guest DMA buffers rather than a capture. `make -C host mediatest` then `tests/mediatest.py` decodes `userdata/Music` and `userdata/Movies` with libmedia and Helix natively and compares with ffmpeg (ADPCM is bit-exact, MP3 about 89 dB).
- **CompactFlash**: `tools/mkcfimg.sh` uses `mkfs.fat` when installed and otherwise mtools' `mformat` (FAT16/32 only). `make -C host fat-test` runs `servers/fs/card.c` and FatFs (with ASan/UBSan) on fresh FAT12/16/32 images, including card swaps and a full volume, then checks each with `fsck.fat -n`. The emulator writes a CF image back only on orderly shutdown.
- **Flash stack**: `out/host/fstest [seed [wear|flips|bad|cuts|format]]` runs `servers/fs/ftl.c` plus littlefs and Dhara on a RAM NAND model with bit flips, failing blocks and power cuts. A full run takes several minutes; pass a phase name to run one.

## Toolchain constraints

- All targets use `-march=mips2 -mabi=32 -G 0 -msoft-float` and are freestanding. Kernel, SPL, interpreter and bootstrap services (fs and net) use `-mno-abicalls -fno-pic`; shared libraries and apps use `-mabicalls -fPIC`. The VR4131 is MIPS III without CLZ or MIPS32 SPECIAL2 ops, and the emulator executes 32-bit code only.
- Nothing links libgcc. Compiler helpers live in `lib/libc` (`divdi3.c` for 64-bit division, `bitops.c` for `__popcountsi2`, `__ctzsi2`, `__clzsi2` and `__bswapsi2`). An "undefined reference to `__…si2`" means another helper is needed there.
- The libc is minimal (`lib/libc/include`). Check it has a function before using it, or add it to `lib/libc/string.c` and similar. The kernel compiles `lib/libc/string.c` and `divdi3.c` too.
- The VR4131 has no LL/SC (VR4131 UM: "does not support LL, LLD, SC, SCD"), so there are no userspace atomics: every lock is a kernel semaphore (`gz_mutex` in `glaze/thread.h`).

## Architecture

Subsystem notes live next to the code, in CLAUDE.md files that Claude Code loads when
working in that directory:

| Directory | Covers |
| --- | --- |
| `kernel/` | boot, flash layout, romfs paging, scheduling, IPC, processes, threads, drivers, CompactFlash, buzzer, battery and power off, OTA |
| `servers/springboard/` | input, the SpringBoard and app protocol, multitasking, passcode, home screen, notifications, hard icons, clipboard, time |
| `servers/fs/` | littlefs and Dhara, ECC, packages, volume layout, `/Card` |
| `servers/net/` | lwIP server and BSD sockets |
| `servers/usb/` | network debugging, USB networking, screen mirroring |
| `servers/audio/` | wave output, player and stream modes, system sounds, libmedia |
| `lib/` | libgfx, libui, handwriting, dynamic linking |
| `apps/browser/`, `apps/wince/` | Browser engine, network and JavaScript; Windows CE apps from NAND |
| `wince/glazeboot/` | RAM boot from Windows CE |
| `packages/` | system packages (hello, taskmanager, maintenance) |

Longer design notes are indexed in `docs/README.md`. Apps, emulators, the website and the
desktop installer are sibling repositories (glaze-apps, glaze-emulators, glaze-web,
glaze-installer) that build against this one; see `docs/REPOS.md` before changing the SDK,
libui/libgfx, `tests/run.sh` or `tools/mkstore.py`, which they use.

## Hardware notes

Register addresses and bit meanings follow the VR4131 and VRC4173 user's manuals and the be300 emulator's models (the `be300` and `mipsel-cross-image` repos hold the manuals, hardware dumps and the Linux port). Cite the manual section in comments when touching hardware code. Some things are known not to match real hardware yet:
- **D-cache colouring:** owned pages, ELF mappings and shared memory match their KSEG0 cache colour, with code publication cache maintenance (shared-memory frame i has colour i & 1 and maps at a VA with bit 12 clear, `kernel/shm.c`; before that, session buffers corrupted network and file data on a real BE-300). Device and resource mappings keep their own policy; the emulator doesn't model cache contents.
- **Factory bad-block marks:** these sit in the spare area and are never read.
- **PCMCIA card setup:** the NE2000 and CF drivers assume the card already decodes I/O at port 0x300, as the emulator's does, and that a CF card decodes the secondary IDE ports. A real card needs its configuration option register written through attribute memory first; 0x0A00D200/D202 is the NAND direct-I/O port, not attribute offset 0x200, so `CF_CIS` only reaches the tuples. After Windows CE the CF data register carries one byte per access, so the driver uses 8-bit transfers.

Learned on a real BE-300 through GlazeBoot (docs/dev/GLAZEBOOT.md has the details), none of which the emulator showed:
- **VR4131 PRId 0x0C80** needs Linux's CACHE-instruction workaround (Config bits 22 and 16), applied in `start.S` before any cached store because it also halves the D-cache line size.
- **Watchdog:** Casio's power-management microcontroller (ScCmcu, 0x0A007800) resets the device 150 s after its last reload; `kernel/drivers/cmcu.c` replays Windows CE's reloads from the tick.
- **GIRQ0 source enables:** bit 3 of 0x0A000010 + 4n enables source n (`KJ_SRC_*`); writing pending bits there is not an acknowledge.
- **Touch:** the PIU needs PADATSTOP, mask 0xDD and a 45 ms scan interval as Windows CE sets them, and the resistive panel's samples are filtered (settle, hold-back, average) so taps don't read as swipes.
- **Serial:** the sync cable's UART (shared with the Stowaway) only works once a Windows CE app has opened `COM1:`, which GlazeBoot does at 115200.
