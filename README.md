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
| `kernel/drivers/` | ICU/GIU interrupt cascade, PIU touch, buttons, RTC, NAND, raw flash for the file server, NE2000 Ethernet |
| `lib/` | Minimal libc, `libgfx` (RGB565 software renderer), `libui` (widgets, keyboard, transitions) |
| `servers/springboard/` | Compositor, lock screen, home screen, app launching |
| `servers/fs/` | File server: littlefs on Dhara on raw NAND, with ECC and bad-block handling (see below) |
| `servers/net/` | Network server: lwIP (TCP, UDP, DHCP, DNS, ICMP) over the NE2000 (see below) |
| `apps/` | Built-in apps, plus `ktest` (kernel self-tests) |
| `assets/` | Fonts (Bitstream Vera, see `assets/fonts/VeraCopyright.txt`) and generated art for romfs |
| `userdata/` | Files the device ships with (sample pictures, a welcome note), built into the file system |
| `host/` | Native libgfx build that renders previews of SpringBoard; `fstest`, the flash stack under fault injection; `mkfsimg`, which builds the factory file system |
| `third_party/` | littlefs, Dhara and lwIP, unmodified (see `third_party/README.md`) |
| `tools/` | NAND image packer, romfs builder, Swift asset generator |

All artwork is original. The device has no radios, so the UI doesn't show
signal or Wi-Fi indicators.

## Building

The cross toolchain comes from the be300 `mips-cross-dev` Docker image:

```sh
./build.sh                 # -> out/glaze.bin (16 MiB NAND image)
./build.sh INIT=bin/ktest  # kernel self-test image
```

The build also compiles `host/mkfsimg` with the host's C compiler (`HOSTCC`,
default `cc`) to build the factory file system from `userdata/`.

To regenerate assets (macOS, needs Swift). The sample photos go to
`userdata/Pictures`, not romfs:

```sh
swift tools/mkassets.swift assets/gen assets/fonts userdata/Pictures
```

## Running

```sh
be300 --nand out/glaze.bin --rtc-host-time
```

The emulator writes NAND back to the image, so work on a copy if you want a
clean file system each run. Add `--ne2000` to insert the emulated PCMCIA Ethernet
card (see Networking).

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
| `0x14000` | Kernel plus romfs (the OS: built-in apps and their assets) |
| `0x500000` | File system (11 MiB, about 6.3 MiB usable): user files and settings |

## File system

Apps reach files through `lib/include/glaze/fs.h` (`fs_open`, `fs_read`,
`fs_list`, ...), which talks to the file server in `servers/fs`. The kernel
starts the server before SpringBoard and gives only it raw access to the
flash above the kernel.

Romfs holds only the OS. Everything else lives in the file system:

| Path | Contents |
| --- | --- |
| `/Documents`, `/Pictures` | The user's files. Photos shows the GZI pictures in `/Pictures`. |
| `/Music`, `/Movies` | Songs (MP3, WAV) for Music and videos (Motion-JPEG AVI) for Videos |
| `/Library/Preferences` | Settings, one file per key (`lib/include/glaze/prefs.h`: `pref_get`, `pref_put`) |
| `/Library/Notes` | Notes: an `index` file and one `<id>.txt` per note |
| `/Library/Caches` | Data apps can rebuild, such as Photos' thumbnails |

A new NAND image already holds a formatted volume with the contents of
`userdata/`, built by `host/mkfsimg` with the same flash code the server
runs, so the first boot just mounts it. Erasing the volume, or booting on
blank flash, leaves only the empty standard folders (`servers/fs/folders.h`).

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

## Networking

With an NE2000 PCMCIA card (`--ne2000` on the emulator), Glaze gets an
address by DHCP and can reach the network. The kernel driver
(`kernel/drivers/ne2000.c`) only moves Ethernet frames, and only for the
network server in `servers/net`, which runs
[lwIP](https://savannah.nongnu.org/projects/lwip/). If DHCP doesn't answer
within ten seconds, it falls back to 10.0.0.1/8 with 10.0.0.254 as router
and DNS server, the addresses of the emulator's NAT gateway.

Apps use `lib/include/glaze/net.h`: `net_resolve`, `net_socket`,
`net_connect`, `net_send`, `net_recv`, `net_bind`/`net_listen`/`net_accept`,
`net_sendto`/`net_recvfrom` and `net_ping`. Calls that wait take a timeout.

- **Settings > Network** shows the card, the address, router and DNS server,
  and pings the router.
- **Browser** (`apps/browser`) loads web pages over HTTP/1.1 and renders
  their HTML, CSS and images. There is no TLS, so https:// pages show a "not
  supported" page.

The emulator's NAT resolves names and opens TCP connections on the host, so
the guest reaches whatever the host can. Without a card, the network server
reports "No Card" and calls fail with `GZ_ENETDOWN`.

## Audio

The BE-300's speaker is fed by a ping-pong DMA block in the companion chip,
not the VRC4173's AIU; its registers come from WinCE's `wavedev.dll`, as
documented in the emulator's `docs/BE300_AUDIO.md`. The kernel driver
(`kernel/drivers/audio.c`) keeps a 64 KiB PCM ring and refills the two DMA
buffers from it, for the audio server in `servers/audio` only. Boot the
emulator with `--audio` to hear it.

Apps use `lib/include/glaze/audio.h`:

- **Player.** `audio_play("/Music/Demo.mp3")`, then pause, resume, seek,
  next and previous. The server decodes the file itself, so music keeps
  playing after the app exits, and it moves on through the folder. It plays
  MP3 (the [Helix](https://github.com/ultraembedded/libhelix-mp3)
  fixed-point decoder) and WAV, either PCM or IMA ADPCM.
- **Stream.** `audio_stream_open(rate, channels)`, then
  `audio_stream_write` with 16-bit PCM at any rate; `audio_stream_clock`
  reports how much has been heard. Opening a stream pauses the player.

The hardware plays 8, 11.025, 16, 22.05, 32 and 44.1 kHz; the server
converts other rates to the nearest one.

- **Music** (`apps/music`) lists the songs in `/Music` and has a Now Playing
  screen with a scrubber and transport controls (OK plays and pauses, Left
  and Right skip).
- **Videos** (`apps/video`) plays Motion-JPEG AVI files from `/Movies`,
  with PCM or IMA ADPCM sound, timed by the sound's clock. Late frames are
  skipped. Reading from flash is the limit: the sample (192x144, 10 fps,
  about 34 KB/s) plays at about 7 frames a second in the emulator. To make
  one:

  ```sh
  ffmpeg -i in.mp4 -vf scale=192:-2 -r 10 -c:v mjpeg -q:v 12 -pix_fmt yuvj420p \
      -ar 22050 -ac 1 -c:a adpcm_ima_wav out.avi
  ```

`host/mediatest` decodes files with the same code natively, and
`tests/mediatest.py` compares it with ffmpeg.

### Stowaway keyboard

Boot with `be300 --nand out/glaze.bin --rtc-host-time --stowaway-keyboard`
(or enable the web emulator's Stowaway option before starting Glaze).
Glaze probes the dock once at boot. After the `FA FD` handshake, Notes,
Files naming dialogs, and Browser addresses/form fields accept
physical input and hide the on-screen keyboard, reclaiming its space. Tap a
field to focus it. Without a detected dock, the existing touch keyboard remains.

The initial layout is US ASCII, with Shift, Caps Lock, editing/navigation keys,
and software key repeat. Enter submits single-line fields and inserts a newline
in multiline editors; Tab advances between Browser text fields. Home moves the
text cursor and does not act as the device's Rocket button. Ctrl/Alt character
combinations and unmapped keys are ignored. Live attach/detach is not supported;
restart to change the dock configuration. The scancode map follows the emulator's
inferred map and still needs validation on a physical Stowaway.

The companion UART is shared with the dock, so kernel logs switch to the VR4131
DSIU after detection. Emulator stdout continues to carry those logs. The SPL's
early companion-UART output is consumed by the dock model when enabled.

Run `make -C host stowaway-test` for native decoder/editor tests with ASan/UBSan.
See [tests/STOWAWAY.md](tests/STOWAWAY.md) for emulator regression checks.
