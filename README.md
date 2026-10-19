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
| `kernel/drivers/` | ICU/GIU interrupt cascade, PIU touch, buttons, RTC, NAND, key/value store |
| `lib/` | Minimal libc, `libgfx` (RGB565 software renderer), `libui` (widgets, keyboard, transitions) |
| `servers/springboard/` | Compositor, lock screen, home screen, app launching |
| `apps/` | Built-in apps, plus `ktest` (kernel self-tests) |
| `assets/` | Fonts (Bitstream Vera, see `assets/fonts/VeraCopyright.txt`) and generated art |
| `host/` | Native libgfx build that renders previews of SpringBoard |
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
