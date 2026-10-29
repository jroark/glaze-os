# USB development on the BE-300

`glaze-usb` is a native libusb command-line tool for stock Windows CE and
Glaze. It can upload and launch a CE executable, boot Glaze through GlazeBoot,
transfer files, launch Glaze apps, read console logs, and reload the kernel
and romfs directly from a running Glaze system.

Paired network debugging exposes the same Glaze runtime commands through
`--host ADDRESS --key KEYFILE`; see [Network debugging](NETWORK_DEBUG.md) for
pairing, Settings controls, encrypted transport and reload reconnection.

## Build and boot

On macOS, install libusb and pkg-config, and have the existing Docker cross
toolchain and `../be300-dev` CE toolchain available:

```sh
brew install libusb pkg-config
./build.sh glazeboot
make glazeboot-exe usb-probe-exe
make -C host glaze-usb

out/host/glaze-usb probe
out/host/glaze-usb ce-self-test
out/host/glaze-usb boot
```

Connect an awake BE-300 with its USB sync cable. `boot` defaults to the
artifacts in `out/glazeboot`; an optional directory argument selects another
build. It first uploads `UsbProbe.exe`, reads it back, launches it with a
unique nonce, and downloads the resulting text to verify execution. Only
after that check succeeds does it upload GlazeBoot and the boot images.
The tool stages files in `\Storage Card\GlazeUSB` when that volume has enough
space, otherwise `\Nand Disk\GlazeUSB`. No card or preinstalled helper is
required. CE uploads are read back before replacing a previous file.

To reclaim an old internal staging copy after moving to CF, boot Windows CE
and run `out/host/glaze-usb ce-clean-nand` (`--dry-run` lists candidates).
It removes only the seven known debugger files in `\Nand Disk\GlazeUSB`,
verifies each deletion and removes the folder only when empty. Other files
and the CF staging copy are preserved. Deletions use CE's filesystem APIs.

`glaze.cfg` supplies `ramfs_kb` and `nand=off` or `nand=ro`. USB boot rejects
`nand=rw` and custom image filenames. The unattended GlazeBoot launch ignores
the confirmation/countdown and `mode` settings and leaves the dock serial
port available for USB. The regular interactive CF loader still works.

On connection, an updated host and USB service supply 32 bytes from the host
OS CSPRNG when the device has no valid Wi-Fi entropy seed. This also happens
after RAM reloads, so WPA/WPA2 can work when the storage card has been replaced
by a Wi-Fi card. An existing seed is preserved for the network service's
ratchet. Seed packets are excluded from `--trace`; the OS random API failing
stops the connection rather than substituting a weaker source. Untethered
boots still use the factory or boot-card seed described in [Wi-Fi](WIFI.md).

The boot runs in RAM. It replaces CE's live RAM, including its object store;
save anything important there before booting. Files staged through CE remain
in its filesystem. USB boot does not flash firmware or write raw NAND.
Return to stock CE with `reboot --ce` on an updated kernel. A full cold reset
also works, as described in [GlazeBoot](GLAZEBOOT.md).

## Commands

The USB cable can also provide an IP connection through a macOS utun or Linux
TAP interface. See [USB networking](USB_NETWORK.md) for `glaze-usb usb-net`.

`glaze-usb mirror` provides live [VNC screen mirroring](MIRRORING.md), including
mouse/stylus and keyboard control, through the same USB or paired TLS link.

```sh
out/host/glaze-usb put out/pkg/hello.gzp /Downloads/hello.gzp
out/host/glaze-usb run /Downloads/hello.gzp
out/host/glaze-usb run bin/settings
out/host/glaze-usb get /Documents/example.txt /tmp/example.txt
out/host/glaze-usb logs             # Ctrl-C stops streaming
out/host/glaze-usb logs --once      # drain a bounded batch
out/host/glaze-usb reload           # fresh kernel + romfs from out/glazeboot
out/host/glaze-usb screenshot out/screen.png
out/host/glaze-usb script tests/unlock.txt out/home.png
out/host/glaze-usb connect          # hold a debugger connection until Ctrl-C
out/host/glaze-usb run bin/settings --wait
out/host/glaze-usb reboot           # restart the current OS
out/host/glaze-usb reboot --ce      # return to stock CE from Glaze
out/host/glaze-usb reboot --glaze   # restart Glaze, or boot it from CE
```

Unlock SpringBoard before launching an app. `run` installs and launches a
`.gzp` package or launches an existing built-in/bundle through SpringBoard,
which manages its surface and lifecycle. It does not launch arbitrary ELF
files. Glaze uploads target its writable root; `/Card` and `/NAND` uploads
are rejected. Downloads use a temporary host file and replace the destination
only after success.

Screenshots are native 240×320 PNGs, including the status bar and any visible
dialog. The device captures the displayed framebuffer atomically, omits LCD
row padding and keeps the snapshot fixed during download. Capturing does not
wake the display or change the foreground app.
If the display is asleep, its blacked-out frame is captured; `script` wakes
the display before replaying input.

`script` accepts the same text format as the emulator's `INPUT_SCRIPT`:

```text
# Times are milliseconds from the start of this script.
0 drag 40 280 235 280 500
1200 tap 120 80
2000 key home
```

Commands are `down`, `move`, `up`, `tap`, `drag`, `key`, and `text`.
Touch coordinates are the emulator's screen coordinates, including the
status bar; calibration tests may deliberately use coordinates outside the
screen. `text` types the rest of the line as ASCII, 40 ms per character.
Named keys include `ok`, `esc`, arrows, `home`, `power`, `enter`, `backspace`,
`tab`, `delete`, `end`, `pageup`, and `pagedown`. Blank/comment-only scripts
are supported. A trailing `#` comment is allowed except after `text`, where
it is literal text.

The entire script is validated before input starts. Up to 2048 events and
one hour are supported; expanded taps, drags and text count toward the event
limit. Events are ordered by timestamp and uploaded before execution. The
device starts its timer 100 ms after accepting START, so USB latency does
not pace gestures. The script clock starts with the command, rather than at
boot as in an emulator test image. Starting a script wakes a sleeping screen
and preserves the normal lock/passcode flow. Input enters the same kernel
event ring as emulator test builds, bypassing the physical touch sensor.
Ctrl-C or session loss cancels queued input and releases held gestures/keys.
An explicit final `down` or `move` stays held through the screenshot, matching
the emulator's mid-gesture test scripts, and is released when the command closes.
An optional output filename saves a PNG after the last event and a short
settling interval. Existing scripts may require the same apps, files, network
or initial UI state as their emulator runs.

The status bar shows a **USB** badge while a debugger session is open:
blue for connected, amber with a transfer arrow while copying files or images,
green with a play symbol while an app launched by the debugger is alive, and
purple during an input script. Copy activity takes priority over app status.
The badge disappears when the session closes; the app may keep running.
`connect` keeps an idle session open. `run PATH --wait` keeps it open until
the launched app exits or Ctrl-C is pressed; Ctrl-C leaves the app running.
Use one CLI command at a time, closing `connect` before starting another.
Fullscreen apps retain their existing hidden status bar.

`reload` validates image sizes, CRCs, kernel entry/layout, romfs entries and
available staging memory before handing over. It stops running apps and
creates a fresh RAM filesystem. If staging or validation fails, the current
OS keeps running. Successful boot/reload reconnects to the same USB port,
prints the running build identifier and drains early console logs.

`reboot` restarts the OS currently running on the device. In Glaze it uses
the same validated fresh-RAM boot as `reload`, taking images from
`out/glazeboot`; running apps and RAM files are discarded. In Windows CE it
sends Casio's native `WilCLBSoftReset` request once, without uploading files.
`--glaze` explicitly chooses Glaze and uses the normal CE bootstrap when
necessary.

`reboot --ce` from Glaze acknowledges the request before stopping USB,
quiesces DMA, and executes a 184-byte uncached reset stub in reserved low
RAM. The stub clears stale retained RAM (including the ROM warm-resume
mailboxes), then requests a VR4131 reset. This forces a fresh CE load from
the unchanged NAND; it does not write NAND or CF. Volatile CE object-store
contents cannot survive a Glaze RAM boot. An older kernel without the CE
reboot capability must first be updated with `reload`. CE takes time to boot;
use `probe`/`ce-probe` after it finishes. `reset-link` only resets the USB bus.

On stock CE, the same `put`, `get`, and `run` commands use Casio PC Connect.
CE `run` accepts an optional command-line string. `ce-info` reports staging
volume space. `ce-probe` performs the harmless PC Connect handshake.

Use `--trace` before the command for packet diagnostics. `probe` reports
the USB location; `--device BUS.PORT` selects one when multiple devices are
attached. `reset-link` requests a USB bus reset. A sleeping device must be
woken before connecting. Closing the tool releases DTR/RTS so CE can end
the session. The handshake drains CE's complete wake-byte train before
sending framed commands; repeatedly tapping Connect Now is unnecessary once
the session is active.

`diag` uses a vendor control request and the interrupt endpoint to report
controller registers and software ring counters without depending on bulk
DMA. It is useful when enumeration works but `probe` cannot complete HELLO.
The boot/reload reconnect path waits up to 90 seconds and retries bus
re-enumeration when macOS retains the preceding OS's USB session. CE may
power down USB while GlazeBoot is still reading CF; a reset during that
interval is retried after Glaze starts. CE-to-Glaze handover and RAM reloads
have been verified to reconnect automatically with the cable left connected.

## Implementation

The controller uses VID:PID `07cf:2002`, bulk IN `81`, bulk OUT `02`, and
interrupt IN `83`. Stock CE advertises device revision `0000`; Glaze uses
`0100`. The host reads the current descriptor at handover rather than relying
on the operating system's cached revision. Stock CE speaks Casio's custom
PC Connect protocol, not standard ActiveSync RAPI.

Glaze uses `GZUB` frames defined in `lib/include/glaze/usb_wire.h`: a 24-byte
little-endian header, up to 4096 payload bytes, request IDs, status and CRC32.
The parser handles fragmented/coalesced frames and resynchronizes after
invalid data. The service caches the preceding response to avoid repeating a
write when that request is replayed. Partial transfers are aborted on session
changes or 30 seconds of inactivity.

The kernel owns bounded RX/TX/log rings and an allocated uncached DMA page.
The trusted service catalog starts dynamically linked `bin/usb` after the file server and grants
its USB and compositor capabilities; ordinary app spawning cannot grant USB
access. Stopping the service cancels queued input and releases held gestures.
Filesystem and SpringBoard operations run in userspace. Console logs survive early boot
and report ring overflow; panic polling can emit logs without the service.
Reload staging reuses GlazeBoot's page relocation trampoline, with metadata
allocated outside all destination ranges. The trampoline stops USB and audio
DMA and flushes caches before replacing the old kernel.

This initial debugger provides transfers, execution, logs and RAM reload.
It does not implement breakpoints, single-stepping, or a GDB stub.

## Validation

On physical BE-300 hardware, the CE upload/read-back/execution probe, USB
GlazeBoot launch, Glaze file round-trip, built-in and uploaded package launch, console logs,
and three consecutive RAM reloads with automatic reconnection have passed.
The screenshot command and existing unlock/mid-drag input scripts have also
passed on hardware, including holding a final touch through capture, starting
a script from a sleeping display, and capturing the running-app status badge.
Reboot has passed both emulator and physical checks: CE soft reset, Glaze
restart with automatic USB reconnection, and a Glaze-to-CE cold reset followed
by a successful CE PC Connect handshake.
The handover keeps companion clocks running while stopping DMA; turning
them off at that point stalled the real device before the second boot marker.
The loader also enables USB's clock before touching its registers, since CE
can power the unit down after its PC Connect carrier is released.

```sh
make -C host usb-wire-test usb-driver-test usb-reload-test usb-pcconnect-test
make -C host usb-input-test usb-script-test
make -C host glaze-usb-emulator
python3 tests/usb_native.py --nand ../be300/ce/restore_images/All_nand_300.bin boot
python3 tests/usb_native.py --nand ../be300/ce/restore_images/All_nand_300.bin --reboot-test ce
python3 tests/usb_native.py --nand ../be300/ce/restore_images/All_nand_300.bin --reboot-test glaze
tools/mkceboot.sh --autorun out/usb-ceboot.img 16 confirm=0 nand=off
python3 tests/usb_runtime.py --nand ../be300/ce/restore_images/All_nand_300.bin \
    --cf out/usb-ceboot.img --reloads 3
```

Native tests cover framing/CRC, packet boundaries, controller state changes,
capability checks, allocation failures and relocation. The driver test uses
the sibling emulator's controller model. The emulator integration tests boot
disposable NAND/CF copies under `out/`, exercise real guest code and repeat
reloads. `glaze-usb-emulator` links a test-only libusb facade; the production
CLI always links real libusb. Integration logs are retained in
`out/usb-native/` and `out/usb-runtime/`.
