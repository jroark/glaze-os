# Screen mirroring and remote control

`glaze-usb mirror` mirrors Glaze's full 240×320 display through a local VNC
bridge on your computer. Use a normal VNC viewer, including macOS Screen
Sharing. Left-button drags act as the stylus, the keyboard types ASCII text,
and special keys map to the BE-300 controls. `--view-only` disables viewer input.

The bridge works over physical USB or the existing paired TLS network debugger.
It requires an updated Glaze kernel, USB service and SpringBoard. No network
card, pairing key or internet connection is needed for direct USB mirroring.
This is screen sharing from Glaze to your computer, rather than a VNC client
for viewing another computer on the BE-300.

Current Glaze builds support Ctrl/Command+A, C, X and V and Shift+Left/Right
or Home/End for editing within Glaze. These use [Glaze's text clipboard](CLIPBOARD.md);
the computer's clipboard is not synchronized and VNC ClientCutText is ignored.

## On a Mac

Build the host tool with Xcode command-line tools and Homebrew dependencies:

```sh
brew install libusb pkgconf
make -C host glaze-usb
out/host/glaze-usb mirror
```

With Glaze running and its USB cable connected, open a second Terminal:

```sh
open vnc://127.0.0.1:5901
```

The first command stays running as the bridge. Close the viewer to release
held input; another viewer can reconnect to the same bridge. Ctrl-C stops the
bridge. If several BE-300s are attached, use the existing `--device BUS.PORT`
selection before `mirror`. The helper does not install anything or require a
kernel extension. USB device permissions still apply (macOS normally permits
libusb access without sudo).

```sh
out/host/glaze-usb mirror --view-only
out/host/glaze-usb mirror --fps 3 --port 5902
```

The default limit is five captures per second; `--fps` accepts 1–15. Actual
frame rate depends on screen changes, device CPU and transport bandwidth. This
is a development/interaction tool; full-screen animation and games can run
below the requested rate. VNC viewers provide their own window scaling.

## Over Ethernet, Wi-Fi or USB networking

First provision [network debugging](NETWORK_DEBUG.md), then:

```sh
out/host/glaze-usb --host DEVICE_IP --key /private/tmp/DEBUG.KEY mirror
open vnc://127.0.0.1:5901
```

For the [USB IP interface](USB_NETWORK.md), keep `glaze-usb usb-net` running
in its own Terminal and use `--host 192.168.7.2`. The tunnel owns the physical
USB handle; mirroring uses paired TLS through that interface. A separate raw
USB debugger command cannot share the handle with a direct USB mirror session.
Normal network/USB debugger ownership rules still apply.

The VNC listener binds **127.0.0.1 only** and uses RFB's None security type.
The local viewer connection is unencrypted and has no separate VNC password;
other local processes can connect when no viewer is active. No VNC port is
opened on Glaze or on the computer's LAN interface. Device authentication and
encryption use the existing paired TLS transport when `--host` is selected.
Do not forward this localhost port to an untrusted network. Screen contents
and typed text are excluded from `--trace` packet dumps.

## Controls and display behaviour

| Viewer input | Glaze input |
| --- | --- |
| Left mouse down / drag / up | Stylus down / move / up, including lock-screen gestures |
| Right mouse button | Escape button |
| Middle mouse button | Home / Rocket button |
| Wheel up / down | Arrow Up / Down taps |
| Printable ASCII key press | Text entry |
| Arrows, Enter, Tab, Backspace, Delete | Matching hardware/keyboard keys |
| Home, End, Page Up / Down | Text navigation keys |
| F1 | Home / Rocket button |
| F2 | Power button |
| F3 | OK button |

On a Mac, function keys may require holding Fn. Unsupported modifier/Unicode
keysyms are ignored. Clipboard messages are consumed without transferring
their contents. There is no clipboard sync, audio streaming, file transfer
within VNC, multi-touch, or desktop resizing. Use the existing file commands
outside the mirroring session for file transfers.

A connected viewer wakes the panel and holds off automatic display sleep while
mirroring is active, including in view-only mode. The status badge shows **VNC**.
The existing lock screen
and passcode remain in place; mirroring does not bypass them. The framebuffer
includes the status bar and foreground app. The viewer uses the same input
ring as physical input, and held keys/touch are released on viewer close,
debugger disconnect, USB reset or service teardown. A reboot/RAM reload ends
the session; restart the bridge after Glaze reconnects.

## Implementation and validation

Glaze takes an atomic RGB565 snapshot using the existing framebuffer-copy
capability, detects changed 16×16 tiles, and serves an immutable delta stream.
The host verifies tile bounds, uniqueness and the reconstructed whole-frame
CRC before updating the viewer. CRC mismatch retries a full frame; an incomplete
transfer needs a full resync. Static screens send metadata only after the first
snapshot. No capture buffer is allocated before a viewer connects.

The bridge implements RFB 3.3, 3.7 and 3.8, Raw rectangles, true-colour
8/16/32-bit formats in either endian order, incremental/subregion requests,
pointer and key events. Unsupported messages and malformed/oversized requests
close that viewer connection. Handshake and socket-write deadlines are bounded.
Only one viewer is served at a time.

```sh
make -C host mirror-test usb-input-test usb-driver-test
make -j4 all glazeboot
```

Native ASan/UBSan tests exercise the production VNC bridge and delta engine
with a protocol client, deterministic framebuffer and input boundary. They
cover all three protocol versions, colour/endian conversion, fragmented
messages, incremental updates, per-region baselines, tile bandwidth, CRC
resync, input, view-only mode, clipboard draining, malformed messages and
held-input cleanup. Kernel tests check input capability gating, batch bounds,
copy faults, all-or-nothing validation and release behaviour.

An optional independent-client check uses `asyncvnc==1.3.0`:
`python3 tests/mirror_vnc.py` after building the native fixture. It verifies
full/incremental RGB output, mouse, text and sparse upstream deltas without
adding dependencies to the regular test target.

The optional `NETDEBUG_TESTS=1 INIT=bin/debug_guest` guest test additionally
checks mirroring through real lwIP/TLS and the development dispatcher, complete
frame CRCs, deltas, live input and close. See [network debugger testing](NETWORK_DEBUG.md).
Rebuild normally afterward to remove the test peer. The cloud is Linux: native
macOS compilation, Apple's Screen Sharing client and physical BE-300 performance
still require validation on a Mac/device.
