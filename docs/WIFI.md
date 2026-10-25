# XI-825 Wi-Fi

Boot a Glaze NAND with the native BE-300 emulator and `--xi825`:

```sh
/path/to/be300 --nand out/glaze.bin --xi825 --rtc-host-time
```

Use a disposable copy of the image for tests. The emulator persists NAND
changes at shutdown. `--xi825` and `--ne2000` select the same primary socket
and cannot be combined. No vendor WinCE driver package is needed by Glaze.

Open **Settings > Wi-Fi**, wait for the scan, and select **BE300**. This is
an open virtual access point on channel 3. The emulator carries its traffic
through the same NAT, DHCP, and DNS backend as NE2000. Settings shows the
SSID, BSSID, firmware signal level, channel, and negotiated rate; scroll for
connection details. Signal is a raw firmware level, not a dBm measurement.
**Settings > Network** shows IP configuration and can ping the gateway.

The first boot remains disconnected. An explicitly selected SSID is saved
in `/Library/Preferences/net.wifi` and reconnected at boot. **Disconnect**
disables this automatic connection, retaining the last choice in the saved
record. Select a network again to reconnect. An unavailable network times
out after ten seconds and stays down until another explicit selection.
DHCP and its ten-second static fallback run only while associated; link
loss clears addresses and DNS. Wi-Fi controls continue working while down.
Secured networks are displayed as unsupported. WEP/WPA, ad-hoc networking,
physical-card validation, and live card replacement are outside this version.

## Interfaces and driver

`glaze/wifi.h` supplies `net_wifi_status`, `net_wifi_scan`,
`net_wifi_connect(ssid, length)`, and `net_wifi_disconnect`. Scan and connect
start operations; poll status for completion and errors. SSIDs are at most
32 bytes with explicit lengths; at most 16 scan results are returned. The
existing fixed-size `net_status` IPC reply is preserved. Its new states
identify disconnected and associating wireless links.

The kernel's `SYS_WIFI_CTL` is restricted to `PROC_NET`; applications use
network-server IPC. Wi-Fi operations on Ethernet or absent cards return
`GZ_ENOENT`. Existing syscall and network request numbers are unchanged.
`gz_netif_info.kind` uses the old reserved halfword to identify the card.

The socket dispatcher identifies supported cards by CIS before NIC writes.
XI-825 is manufacturer `0xD601`, product `0x0005`, function network; the
emulator's NE2000 is `0x0149:0x0200`. CF storage keeps its ATA driver.
PRISM initialization configures open infrastructure mode and uses the
resident station firmware. RX/TX use bounded BAP/FID transfers with odd-byte
handling, events, and TX reclaim. Commands have bounded polling deadlines;
IRQ and syscall register sequences are serialized on the single CPU.

Definitions follow the local BE-300 emulator's `docs/XI825.md` and
`src/hw/xi825.c`, which cite the Linux hostap HFA384x definitions and vendor
driver observations. The COR uses attribute byte `0x3E0`; it does not write
the NAND direct-I/O alias at `0xD200`.

## Verification

```sh
make -C host netdev-test xi825-test settings-test
# Override XI825_MODEL=/path/to/be300 for the independent PRISM model tests.
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy XI825=1 tests/run.sh tests/t_wifi.txt 22
# Rebuild with a different script but reuse the first run's saved preference:
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy XI825=1 \
  FS_FROM=out/shots/t_wifi/glaze.bin tests/run.sh tests/t_wifi_reboot.txt 15
```

The runner resolves the sibling emulator through Git's original checkout,
including from a temporary worktree; `BE300` overrides that binary.
Both logs, disposable NAND, and a screenshot are saved under `out/shots/`.
The reboot log must show `wifi: saved network association 0` followed by DHCP
without selecting a scan result. `NO_BUILD=1` reuses an existing build.

`./build.sh INIT=bin/ktest` includes native Wi-Fi integration checks when
booted with `--xi825`: scan, invalid SSIDs, association, IP/UDP/TCP/DNS,
disconnect cleanup, close while down, timeout without a DHCP fallback,
reconnection, and persisted preferences. Internet HTTP and NTP are reported
as informational checks because host connectivity can vary. Repeat with
`--ne2000`, no card, and a CF storage image for compatibility validation.

Validated with the native emulator: first-use selection and router ping,
reconnection after reboot (channel 3, 11 Mbps), and Browser loading
`http://example.com/`. Sanitized host tests pass; guest `ktest` reports
`ALL PASS` with XI-825, NE2000, CF storage, and no card.
