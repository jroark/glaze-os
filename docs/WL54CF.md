# WL54-CF wireless networking

Glaze has a native MIPS driver for the AmbiCom WL54-CF's Marvell CF8385
station interface. Settings > Wi-Fi selects open, WEP, WPA Personal, and WPA2 Personal networks.
See [WIFI.md](WIFI.md) for security capabilities, credentials, and validation. It exposes
ordinary Ethernet frames through the existing `SYS_NET_*` ABI and lwIP
network server, so applications and Settings > Network use the same network
APIs as NE2000.

## Local firmware and build

The supplied AmbiCom package contains an ARM/Thumb CE 4.1 driver, which cannot
execute on the MIPS BE-300. Its embedded ARM **card firmware** is independent
of the host CPU, however, and is uploaded by the new Glaze driver.

```sh
python3 tools/extract_wl54cf_firmware.py /path/to/wl54gcfppc41/wince41.cab
./build.sh WL54CF=1
cp out/glaze.bin out/wireless-test.bin
/path/to/be300 --nand out/wireless-test.bin --wl54cf --rtc-host-time
```

The extractor needs `cabextract` for CAB input; it also accepts the extracted
`AMWL54G.dll`. It verifies the exact vendor DLL and helper/main image hashes,
then writes `firmware/wl54cf/cf8385_helper.bin` and `cf8385.bin`. These assets
are ignored and must not be committed. `WL54CF_FW_DIR` overrides their build
location. Ordinary builds do not need firmware; `WL54CF=1` requires both files
and packs them into romfs under `firmware/`.

After selecting a network or restoring a saved choice, serial milestones include:

```text
wl54cf: firmware ready
wl54cf: MAC ...
wl54cf: ready (disconnected)
wifi: associated, starting DHCP
net: DHCP address 10.0.0.1, router 10.0.0.254, dns 10.0.0.254
```

`--net-mac` selects the station MAC. NE2000 and WL54-CF claim the same socket.
The driver automatically identifies the card through its CIS before touching
NE2000 registers. A recognized wireless card remains reserved even if firmware
is missing or initialization fails, so the ATA driver cannot reset it.

## Driver and hardware evidence

The register/handshake/descriptor definitions follow Linux v6.6 Libertas
[if_cs.c](https://github.com/torvalds/linux/blob/v6.6/drivers/net/wireless/marvell/libertas/if_cs.c),
[host.h](https://github.com/torvalds/linux/blob/v6.6/drivers/net/wireless/marvell/libertas/host.h),
[cfg.c](https://github.com/torvalds/linux/blob/v6.6/drivers/net/wireless/marvell/libertas/cfg.c),
[tx.c](https://github.com/torvalds/linux/blob/v6.6/drivers/net/wireless/marvell/libertas/tx.c) and
[rx.c](https://github.com/torvalds/linux/blob/v6.6/drivers/net/wireless/marvell/libertas/rx.c).
The corresponding emulator's `docs/WL54CF.md` records the firmware offsets,
SHA-256 values, CIS assumptions and exact emulation boundaries.

The driver configures COR, uploads helper/main firmware through the command
FIFO, verifies scratch readiness, and obtains the station MAC. It starts
disconnected; runtime Wi-Fi controls scan and associate with a selected AP. It then enables RX,
TX and broadcast reception. Timeouts, firmware requests, command identity,
sequence, response size/result, beacon bounds and RxPD lengths are checked.
FIFO transfers handle odd trailing bytes without unaligned MIPS accesses.
Runtime RX converts 802.3/LLC/SNAP to Ethernet II before queueing for lwIP.

`kernel/drivers/net.c` owns permissions, the bounded receive queue, IPC
notifications and statistics for both card drivers. NE2000 remains available
through the same syscall ABI. Runtime commands and IRQ FIFO transfers are serialized. A card initialization failure leaves card
interrupts masked. Physical removal is outside this version's scope.

**Hardware limitation:** COR uses the existing emulator byte-addressed
attribute alias at `0x0b400100`; that alias/bridge configuration still needs
physical BE-300 validation. `0x0a00d200` is deliberately avoided because it is
NAND direct I/O on this board. The minimal CIS/B1 revision are model choices,
not a captured physical card EEPROM. This is not yet a claim that the driver
operates a real WL54-CF.

Live replacement and physical-card interoperability remain unvalidated. The emulator's station implementation
provides the virtual AP and existing NAT backend.

## Tests

```sh
make -C host net-driver-test
make -C host wl54cf-driver-test BE300_SOURCE=/path/to/be300-worktree
./build.sh WL54CF=1 INIT=bin/ktest
```

Both native targets run with AddressSanitizer and UndefinedBehaviorSanitizer.
The first checks backend selection, failed-card reservation, unchanged syscall
permissions, queue backpressure, notifications, statistics and minimum frame
padding. The second runs the production driver against the emulator model,
including missing firmware, firmware/command timeouts, stale sequences,
oversized/inconsistent response lengths, malformed scan records, unsupported
silicon revision, RX descriptor bounds and odd-length packet conversion.
It uses structural firmware fixtures and requires no vendor binaries.

Boot a copy of the kernel-test image with `--wl54cf --rtc-host-time` and require
`ktest: ALL PASS`. Repeat with `--ne2000` and no card for regressions. Networking
tests require DHCP/router reachability and include optional Internet HTTP/DNS
checks. Keep stdout, stderr and the orderly-shutdown screenshot.

For a local HTTP fixture and Browser screenshot:

```sh
python3 tests/run_wl54cf.py --emulator /path/to/be300 \
  --host-ip <host-LAN-IPv4>
```

This builds a SpringBoard image with scripted Browser input, starts a temporary
HTTP server bound to that host interface, boots a disposable NAND copy, requires
firmware/association/DHCP milestones and the unique HTTP request, checks the
source image hash, and saves logs/screenshots under `out/wl54cf-*/`. Use an
address reachable by the emulator NAT, not `127.0.0.1` (lwIP routes loopback
inside the guest). On macOS, use the native Python runtime if the legacy
runtime cannot receive connections on the chosen interface.

`NET=wl54cf BE300=/path/to/be300 tests/run.sh tests/t_network.txt 15` selects the
card for existing UI scripts and includes firmware automatically. `NET=1`
continues to select NE2000.

Validated 2026-10-25: the native dispatch and peer-model tests passed with
ASan/UBSan. Glaze kernel tests reported zero failures with WL54-CF, NE2000,
no card and CF storage. The wireless boot obtained DHCP, resolved DNS and
fetched Internet HTTP. The local Browser fixture rendered **WL54-CF HTTP
PASS**, and its source NAND hash remained unchanged. The matching emulator
passed all 14 asset-free CTests and the default WinCE 3.0 cold-boot regression.
