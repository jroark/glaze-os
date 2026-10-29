# Network development debugging

The Glaze development service accepts the same framed debugger protocol over
USB and paired TLS/TCP. Network debugging is **off by default**. It needs an
established Ethernet, Wi-Fi or USB network connection, and a pairing record.
No listener or TLS context is opened while disabled or without a network.

The [USB network interface](USB_NETWORK.md) can supply that connection without
a network card. Start `glaze-usb usb-net` on macOS or Linux, then connect the
paired debugger to `192.168.7.2` in another terminal.

## Build and pair

Build Glaze normally, including the updated shared wolfSSL library. The `usb`
service is now dynamically linked; include it and `libwolfssl.so.1` in custom
RAM-boot images. Build the existing host CLI with:

```
make -C host glaze-usb
```

Create a pairing record on the host using its OS CSPRNG. The command creates a
new 64-byte file with mode 0600 and refuses to overwrite an existing file:

```
out/host/glaze-usb net-keygen /tmp/DEBUG.KEY
out/host/glaze-usb net-enable /tmp/DEBUG.KEY
```

`net-enable` uses the physical USB connection to provision the key and enable
the network listener. Pairing bytes are excluded from packet tracing.
For pairing without USB, transfer `DEBUG.KEY` to Documents or Card, then open
**Settings → Debugging → Import DEBUG.KEY** and enable **Network Debugging**.
The page shows the device address and whether a key is installed. Remove the
transfer copy afterward and keep the computer's key file private.

Choose the host and key before the command:

```
out/host/glaze-usb --host 192.168.1.42 --key /tmp/DEBUG.KEY probe
out/host/glaze-usb --host 192.168.1.42 --key /tmp/DEBUG.KEY put out/pkg/hello.gzp /Downloads/hello.gzp
out/host/glaze-usb --host 192.168.1.42 --key /tmp/DEBUG.KEY run /Downloads/hello.gzp
out/host/glaze-usb --host 192.168.1.42 --key /tmp/DEBUG.KEY get /Documents/example.txt /tmp/example.txt
out/host/glaze-usb --host 192.168.1.42 --key /tmp/DEBUG.KEY logs --once
out/host/glaze-usb --host 192.168.1.42 --key /tmp/DEBUG.KEY screenshot out/screen.png
out/host/glaze-usb --host 192.168.1.42 --key /tmp/DEBUG.KEY script tests/unlock.txt out/home.png
out/host/glaze-usb --host 192.168.1.42 --key /tmp/DEBUG.KEY run bin/settings --wait
out/host/glaze-usb --host 192.168.1.42 --key /tmp/DEBUG.KEY connect
out/host/glaze-usb --host 192.168.1.42 --key /tmp/DEBUG.KEY reload
out/host/glaze-usb --host 192.168.1.42 --key /tmp/DEBUG.KEY reboot --ce
out/host/glaze-usb --host 192.168.1.42 --key /tmp/DEBUG.KEY net-disable
```

The default port is 3000; `--host ADDRESS:PORT` can select a forwarded host port.
The device listens on port 3000. It supports IPv4 and host-side DNS names.
Disable access at any time in Settings or with `net-disable`; that closes the
listener and active session. Importing a new key disables access until explicitly
re-enabled. A single debugger owns the shared dispatcher at a time: close USB
`connect` before connecting over the network, or close the network command
before using USB. A network session displays **NET**, with the same transfer,
app-running and script activity colors as the USB badge.

## Feature parity and scope

| Glaze runtime feature | USB | Paired network |
| --- | --- | --- |
| Verified upload / download and interrupted-transfer cleanup | Yes | Yes |
| Install and launch packages, built-ins and existing bundles | Yes | Yes |
| Console logs, including retained early-boot logs | Yes | Yes |
| Atomic 240×320 screenshots | Yes | Yes |
| Timed input scripts, wake, status, cancellation on disconnect | Yes | Yes |
| Session/app activity badge, connect, run --wait | Yes | Yes |
| Validated fresh-RAM reload / Glaze restart | Yes | Yes |
| Return to Windows CE | Yes | Yes |
| Host CSPRNG entropy provisioning | Yes | Yes |

The filesystem boundaries, locked-screen launch restrictions, image validation,
script limits and request replay handling are shared with [USB](USB.md).
This is development automation, without GDB breakpoints or single-stepping.

Stock Windows CE's PC Connect commands (`ce-*`, CE executable upload/boot),
`reset-link`, and the USB controller's `diag` remain USB transport operations.
There is no network agent in stock CE. After `reboot --ce`, the network debugger
is gone; use USB PC Connect or the existing boot-card loader to start Glaze again.

## Reload and connection lifetime

A Glaze reload preserves the enabled debugger's pairing, fresh entropy and
saved Wi-Fi profile in a bounded, versioned block in the boot-info page. Only
the trusted development service can set or consume it. Restore consumes and
wipes the block; normal boots and disabled sessions carry no debugger state.
This preserves reconnect capability while all other RAM filesystem contents
are reset. Save important user files before a reload, as with USB.

The host retries the same address for up to 90 seconds after reload/reboot.
Use a stable DHCP reservation or known address; there is no discovery protocol
or automatic address-change search. Both kernels must support this handoff.
Uploading an older image can remove the network listener and require USB again.

A disconnected peer, interface loss, disabled setting, changed pairing key,
handshake timeout or idle session releases transfer staging, screenshot memory,
and held/scripted input. The normal 30-second debugger idle limit applies;
`connect`, `logs`, and `run --wait` send requests to keep the session alive.
USB cable state changes do not terminate an active network session.

## Transport and keys

Network frames are carried inside wolfSSL TLS 1.2 with the explicit
`ECDHE-PSK-AES128-GCM-SHA256` suite and the `glaze-debug-v1` PSK identity.
There is no plaintext command endpoint, certificate fallback, or anonymous
command dispatch. Wrong keys fail before a GZUB session can start. Browser
HTTPS clients continue verifying certificates and host names; they do not set
PSK callbacks. The library's added server/PSK capability is used by this service.

The pairing file contains a 32-byte random PSK followed by a separate 32-byte
seed. The device ratchets its private debugger seed on durable storage before
using RNG output. It does not race the Wi-Fi entropy preference. The key is
stored as plaintext in Glaze preferences, which currently lack a per-app
keychain/isolation boundary. Anyone with the pairing file can exercise the full
development interface. Keep it out of Git and shared folders; default
`DEBUG.KEY` exports are ignored. No keys or seeds appear in normal diagnostics.

## Validation

```
make -C host debug-net-test
make -C host usb-wire-test usb-driver-test usb-reload-test usb-input-test usb-script-test
make -C host usb-pcconnect-test usb-cleanup-test browser-test
```

Native tests run the production device TLS state machine on loopback sockets
against the production host transport: disabled listener, private/exclusive key
creation, wrong keys, rejected plaintext, fragmented/coalesced records, seed
ratcheting, disabling an active connection, key rotation and invalid configuration. ASan/UBSan covers the device
fixture. Existing USB driver, protocol, reload, input, script, PC Connect and
cleanup tests remain applicable. The reload test checks handoff preservation,
one-shot consumption, wiping and allocation cleanup.

For real guest integration, choose a host LAN address reachable through the
emulator's NAT, build the optional test peer, then run inside the toolchain:

```
make -j4 NETDEBUG_TESTS=1 INIT=bin/debug_guest DEBUG_RELOAD_ADDR=HOST_LAN_IP out/glaze.bin glazeboot
python3 tests/debug_runtime.py --host HOST_LAN_IP --emulator ../be300/build-host/be300
```

The test peer uses known synthetic credentials only in this opt-in image.
Debugger traffic goes through lwIP's loopback path to the device's own address;
the host serves only the reload image stream on port 8877. This accommodates
the emulator NAT's lack of incoming TCP forwarding. It covers the production
service dispatcher, files/replay, logs, UI launch/status, screenshot, timed
input, disconnect cleanup and a RAM reload followed by restored pairing and a
new TLS session. NAND is copied under `out/debug-network-runtime/`.
Build normally afterward to remove the test peer and restore SpringBoard as init.
Physical-device network debugging still needs validation.
