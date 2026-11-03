# CF Bluetooth discovery and serial

Glaze supports controller initialization and classic Bluetooth discovery with
the SMART/Brainboxes CF card carrying FCC ID NXXBL-565. Settings → Bluetooth
shows controller status, address/version, Scan/Cancel, and up to 16 devices.
Select a discovered device to connect its serial service with a 1–16 character
PIN and either automatic SDP selection or an explicit channel. Active sessions
show their peer, connection progress and security, with Disconnect or Cancel
Connection available even for sessions opened over USB.
Names are resolved sequentially after inquiry; unavailable names retain their
addresses. The service also supports one outbound, legacy-PIN-authenticated,
encrypted Serial Port Profile connection through the SDK and USB diagnostics.
HID, file transfer, audio, BLE and persistent Glaze bonds
are outside this version.

## Verified hardware

The physical card on a BE-300 supplied these CIS tuples on 2026-11-02:

- MANFID `0160:aba0`, FUNCID `02` (serial).
- VERS_1 `Brain Boxes`, `CF+ Bluetooth_Card, BCSP`, `r9`.
- CONFIG register address `0xe0`, last index `3`, mask `07`.
- CFTABLE_ENTRY bodies `c5 01 99 07 b5 1e b5 1e 55 23 70 ff ff 20`
  and `03 08 a3 60 f8 03 07`.

Use entry 3, COR `43h`, UART I/O port `3f8h` (eight registers), divisor 1,
8E1, FIFO enabled with trigger one, and MCR `0bh`. The socket layer owns
power/bus setup; the UART driver preserves that setup. The absolute clock/baud
was not measured: divisor 1 is the verified programming value. Divisor 8
produced invalid bytes. The default CIS entry advertises index 5 despite CONFIG
last-index 3; do not blindly use that default instead of verified entry 3.

This is BCSP, not H4. A bounded probe received SYNC responses
`c0 00 41 00 be ac af ef ee c0`. Full service testing subsequently completed
SYNC/CONF negotiation, HCI Reset, local version/features/address reads, inquiry,
remote name resolution, repeat scans and inquiry cancellation. The controller
reported Bluetooth 1.1, CSR manufacturer ID 10 and revision `020dh`. Two nearby
devices were discovered and their names resolved on the real hardware.

Cold NAND boot, physical card swaps and hibernate/resume still need a hardware
acceptance pass. The socket lifecycle paths have native regression coverage;
physical validation so far used Glaze USB RAM reload from an existing Glaze
session with the card inserted.

## Authenticated serial connections

`servers/bluetooth/serial.c` adds basic-mode L2CAP signaling/configuration,
bounded HCI ACL fragmentation/reassembly, SDP ServiceSearchAttribute with
continuation, and RFCOMM PN/SABM/MSC and credit-based flow control. HCI ACL
credits come from Read Buffer Size and Number Of Completed Packets. Security
uses the controller's legacy PIN/link-key operations, followed by explicit
connection encryption. Serial data is not exposed until authentication and
encryption have succeeded. One selected peer and two L2CAP channels are supported.

`bt_serial_connect` discovers an SPP channel with SDP. It prefers a record named
`Glaze Serial Echo` when one is returned, otherwise uses the first SPP record.
`bt_serial_connect_channel` selects a known RFCOMM channel explicitly. Both
return an asynchronous, port-bound handle used by `bt_serial_write`,
`bt_serial_read`, and `bt_serial_disconnect`; `bt_serial_status` reports progress,
controller error, authentication/encryption, and byte counters. Reads/writes
transfer up to 48 bytes per IPC request. There is one active serial session;
discovery cannot interrupt it. Obsolete handles cannot target a restarted service.
`bt_serial_disconnect_current` observes the current session and closes it using
the same service port and session identifier, so Settings can manage another
client's connection without closing a replacement session.

The status bar reserves the CF card glyph for mounted storage. Separate Wi-Fi,
wired-network and Bluetooth glyphs indicate their own transports; Bluetooth is
blue while the serial connection is open and muted while the card is available.
Each icon opens its own details, and USB networking can appear beside a card's
storage or Bluetooth icon. Battery percentage and debugger status retain their
own space.

PINs and a single peer's link key are kept only in service RAM; changing peers
clears the cached key. Restarting Glaze or the Bluetooth service loses the bond.
macOS may retain its own system pairing record. Raw packet tracing is disabled
before serial pairing starts, and diagnostic tools never write PINs/keys to files.
After a Glaze reboot, a stale Mac bond can time out without requesting a PIN;
forget this test card in macOS Bluetooth settings before pairing it again.

Hardware testing on 2026-11-03 completed legacy pairing and encryption with this
Mac and verified exact 31-byte and 192-byte serial echoes. Native tests also
exercise SDP continuation, multiple-service selection, small ACL fragments,
credit starvation/replenishment, repeated security events, malformed records,
FIFO bounds, disconnect and failure recovery. See
[the Mac echo helper](../../tools/bt_echo/README.md) for reproducible commands.
The expanded Settings page was also tested on the BE-300: Disconnect closed a
USB-opened, encrypted session, the Mac reported disconnection, and the service
returned to Idle while the Bluetooth glyph changed from blue to muted.

## Driver and service

Only the exact MANFID/FUNCID/VERS_1 match loads `drivers/btuart.ko`; other serial
cards remain candidates and never receive ATA or network probes. The module
owns bounded RX/TX rings and services RX, line-status and THRE interrupts through
the existing PCMCIA cascade, with timer polling as a fallback. UART errors stop
that source; service recovery resets the UART. Removal masks the source before
unloading, invalidates the generation and stops the service on the next worker
pass. Missing/invalid modules leave the socket reserved.

`bin/bluetooth` has a dedicated `bluetooth` capability and hardware dependency.
It owns BCSP and HCI in one event loop. BCSP uses SLIP, header checksums,
optional incoming CRC, outgoing CRC, a one-packet reliable window, sequence/ACK,
retransmission and link reset detection. Both commands and events use BCSP
channel **5**; channel 6 is ACL. Early unsolicited controller events may be
retransmitted after link establishment. The service retries failed initialization
after five seconds. No vendor firmware or persistent controller settings are
written.

Inquiry lasts about ten seconds. Remote name requests have two-second deadlines,
then are canceled before proceeding; names are bounded to 39 printable bytes.
Results are deduplicated by address. Failed operations, failed links and a scan
with no results are distinct states. A total deadline bounds credit starvation.
Status snapshots use a revision and generation to reject mixed results while
scanning. The event loop keeps IPC and cancellation responsive during discovery.

## Interfaces and diagnostics

`glaze/bluetooth.h` exposes `bt_status`, `bt_scan` and `bt_cancel`. Scan/cancel
start asynchronously. Status includes controller metadata and devices, and may
return `GZ_EBUSY` when results change during collection; retry on a later tick.
Addresses use HCI order, least significant octet first. App requests use a
named service port and fixed-size IPC replies; raw UART operations are restricted
to the kernel-granted Bluetooth capability and the process's card generation.

`SYS_BT_CARD_INFO(buffer, size)` provides a cached read-only version-1 CIS
snapshot. It requires the exact structure size, clears evidence on removal/resume,
and never reconfigures hardware. `function` is UINT32_MAX when unavailable;
generations are invalidation tokens, not insertion counters.

Host commands:

```sh
out/host/glaze-usb bt-info
out/host/glaze-usb bt-status
out/host/glaze-usb bt-scan
out/host/glaze-usb bt-scan --start-only
out/host/glaze-usb bt-cancel
```

USB capability negotiation rejects older builds. `bt-info` uses nine explicit
little-endian words, 256 CIS bytes and 96 identification bytes. Discovery status
uses a 48-byte explicit header followed by 52 bytes per device. These formats
do not transmit native structure layouts. `BT_DEBUG=1` enables bounded UART
packet tracing in diagnostic builds; normal builds omit it.

## Validation

```sh
make -C host bluetooth-test bluetooth-serial-test bluetooth-driver-test bluetooth-client-test connectivity-test cf-boot-test settings-test \
    service-test net-driver-test boot-init-test
python3 tests/services_catalog.py
./build.sh all glazeboot GB_APPS=all HWDUMP=1
```

Native tests cover recorded controller traffic, fragmented frames, CRC/checksum
rejection, duplicates, sequence wrap, retransmissions, partial writes, link reset,
HCI initialization, credit starvation, inquiry, deduplication/result caps, names,
name timeout/cancel, inquiry cancel, module validation, copy faults, generations,
removal/resume, service restart/dependencies, Settings pairing/disconnect/error
handling and scrolling, and transport-specific icon presence/layout.
The existing module-loader native harness additionally needs a host supporting
its low 32-bit mappings; those mappings are unavailable on this Mac/container.
The real BE-300 successfully loaded and executed the new module, and the
separate transport-boundary tests cover its descriptor validation and rollback.

Settings, Bluetooth, USB, SpringBoard and Files compile with `-Os` to preserve the fixed
4.75 MiB ROM archive limit without removing default apps.

References:
- [Brainboxes BL-565](https://www.brainboxes.com/product/discontinued/bl-565)
- [Linux BCSP implementation](https://github.com/torvalds/linux/blob/v6.1/drivers/bluetooth/hci_bcsp.c)
- [Bluetooth Core, HCI functional specification](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/host-controller-interface/host-controller-interface-functional-specification.html)
- PC Card Standard tuple definitions; the captured CIS above is the hardware
  source for this match/configuration.
