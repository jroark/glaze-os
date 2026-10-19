# Glaze

A small microkernel OS for the Casio BE-300 (NEC VR4131 MIPS, 240x320 LCD) with
a user interface in the style of early iPhone OS. It runs on the
[be300 emulator](https://github.com/jroark/be300) and boots through the stock
boot ROM from NAND.

## What's here

| Path | Contents |
| --- | --- |
| `boot/` | Stage-1 SPL loaded by the boot ROM; it streams the kernel from NAND |
| `kernel/` | Exceptions, TLB/page tables, scheduler, IPC, shared memory, ELF loader, romfs, syscalls |
| `kernel/drivers/` | ICU/GIU interrupt cascade, PIU touch, buttons, RTC, NAND, key/value store, raw flash for the file server |
| `lib/` | Minimal libc, `libgfx` (RGB565 software renderer), `libui` (widgets, keyboard, transitions) |
| `servers/springboard/` | Compositor, lock screen, home screen, app launching |
| `servers/fs/` | File server: littlefs on Dhara on raw NAND, with ECC and bad-block handling (see below) |
| `apps/` | Built-in apps, plus `ktest` (kernel self-tests) |
| `assets/` | Fonts (Bitstream Vera, see `assets/fonts/VeraCopyright.txt`) and generated art |
| `host/` | Native libgfx build that renders previews of SpringBoard; `fstest`, the flash stack under fault injection |
| `third_party/` | littlefs and Dhara, unmodified (see `third_party/README.md`) |
| `tools/` | NAND image packer, romfs builder, Swift asset generator |

All artwork is original. The device has no radios, so the UI doesn't show
signal or Wi-Fi indicators.

## Building

The cross toolchain comes from the be300 `mips-cross-dev` Docker image:

```sh
./build.sh                 # -> out/glaze.bin (16 MiB NAND image)
./build.sh INIT=bin/ktest  # kernel self-test image
```

To regenerate assets (macOS, needs Swift):

```sh
swift tools/mkassets.swift assets/gen assets/fonts
```

## Running

```sh
be300 --nand out/glaze.bin --rtc-host-time
```

The emulator writes NAND back to the image, so work on a copy if you want a
clean store each run.

For unattended UI tests, build with `INPUT_SCRIPT=tests/calc.txt`. The kernel
then replays the timed touch and key events in that file (see
`kernel/drivers/input.c`).

Scripted input skips the touch panel. To test the real PIU and interrupt
path, `tests/touch.sh` drives the emulator window with xdotool on an X display
(Linux, needs Xvfb, xdotool and ImageMagick):

```sh
Xvfb :99 & DISPLAY=:99 tests/touch.sh tests/touch_unlock.act
```

## Memory map

| NAND offset | Contents |
| --- | --- |
| `0x0` | Partition table |
| `0x4000` | SPL (B000FF container) |
| `0x14000` | Kernel plus romfs (apps and assets) |
| `0x500000` | Key/value store (two 512 KiB log areas) |
| `0x600000` | File system (10 MiB, about 5.7 MiB usable) |

## File system

Apps reach files through `lib/include/glaze/fs.h` (`fs_open`, `fs_read`,
`fs_list`, ...), which talks to the file server in `servers/fs`. The kernel
starts the server before SpringBoard and gives only it raw access to the
flash above the key/value store.

The server runs [littlefs](https://github.com/littlefs-project/littlefs) on
the [Dhara](https://github.com/dlbeer/dhara) flash translation layer
(`servers/fs/ftl.c`):

- **ECC.** The spare area can't be used, because the emulator synthesizes it
  and its images hold page data only. Each page gets a 4-bit BCH code
  instead, kept in the unused tail of the checkpoint page Dhara writes after
  every three data pages.
- **Bad blocks.** Programs and erases are verified by reading back. A block
  that fails is retired into a bad-block table kept in the first two blocks
  of the region.
- **Power loss.** Dhara commits on littlefs's syncs. A checkpoint cut off
  mid-write is treated as never written.

Factory bad-block marks are not read yet, since they live in the spare area.
Real hardware needs that before trusting the region.

`host/fstest` runs the same code against a RAM model of the flash with bit
flips, failing blocks and power cuts (about five minutes):

```sh
make -C host fstest && host/fstest 1
```

Files (`apps/files`) browses the volume. It opens text files and pictures,
and its Edit mode makes, renames and deletes folders and files.
