# WL54-CF wireless networking

Glaze has a native MIPS driver for the AmbiCom WL54-CF's Marvell CF8385
station interface. Settings > Wi-Fi selects open, WEP, WPA Personal, and WPA2 Personal networks.
See [WIFI.md](WIFI.md) for security capabilities, credentials, and validation. It exposes
ordinary Ethernet frames through the existing `SYS_NET_*` ABI and lwIP
network server, so applications and Settings > Network use the same network
APIs as NE2000.

## Firmware and build

Linux also uploads helper and main firmware into the CF8385; an open source
host driver does not replace the card's firmware. The pinned Linux pair is
redistributable in binary form for use **only with Marvell silicon products**,
provided its copyright, conditions and disclaimer accompany it. No modification,
reverse engineering, decompilation or disassembly is permitted. Read the full
[LICENCE.Marvell](../firmware/LICENCE.Marvell).

Glaze pins linux-firmware revision
[`ba2549fc973076ac1500a9ba5adf8864c130e7f5`](https://kernel.googlesource.com/pub/scm/linux/kernel/git/firmware/linux-firmware.git/+/ba2549fc973076ac1500a9ba5adf8864c130e7f5/).
Its [WHENCE](https://kernel.googlesource.com/pub/scm/linux/kernel/git/firmware/linux-firmware.git/+/ba2549fc973076ac1500a9ba5adf8864c130e7f5/WHENCE)
identifies both `libertas/cf8385.bin` and `libertas/cf8385_helper.bin` as
redistributable under that license, extracted with Marvell's permission.
[The manifest](../firmware/wl54cf-linux.json) records their sizes and SHA-256
values. Fetch them without a vendor CAB:

```sh
python3 tools/fetch_wl54cf_firmware.py
./build.sh glazeboot INIT=bin/springboard INPUT_SCRIPT= GB_APPS=all WL54CF=1
make glazeboot-exe BE300_DEV=/path/to/be300-dev
```

The fetcher verifies every download against the pinned manifest before writing
anything. Binary files remain ignored under `firmware/wl54cf/`.
`WL54CF_FW_DIR` overrides their build location. Ordinary builds do not need
firmware; `WL54CF=1` requires both files and packs them under `firmware/`.
Redistributed builds must include the full license notice and provenance;
`tools/package_glazeboot.py` enforces the exact pair and adds these notices.
The public emulator NAND omits firmware: this license is limited to Marvell
silicon, not an emulated card. Driver/model tests use synthetic fixtures.

The Linux main image (97,128 bytes) is byte-identical to the one extracted from
AmbiCom's package. Its helper is 2,140 bytes, versus 2,136 bytes in that package,
and is different; neither is modified to match the other. The Linux pair fits
Glaze's bounded, even-length helper/main upload interface. Physical-card
interoperability of the Linux helper remains unvalidated; inclusion in a
release is not a hardware validation claim.

For comparison with previous private experiments,
`tools/extract_wl54cf_firmware.py` still accepts the supported vendor CAB or
`AMWL54G.dll` (CAB input needs `cabextract`) and verifies its exact hashes.
The public packager rejects that different helper. No vendor DLL is bundled.

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
not a captured physical card EEPROM.

**On a real card (2026-10-31).** A WL54-CF inserted into a BE-300 booted from
NAND (`NAND_DROP=wince WL54CF=1` with the pinned Linux pair) came up with the
Linux helper: its CIS reads `2df:8103` (revision 10, the emulator models 12),
COR 0x41 at attribute 0x200 reads back, `firmware ready`, the station address
from the CIS (`00:02:2d:...`), then association with the user's network and a
DHCP lease. Which security mode that network used is not recorded, and
removal, WEP/WPA/WPA2 and throughput are not yet checked on hardware.

The card can be inserted
and pulled at run time (`kernel/drivers/cf.c`'s `pcmcia_poll`, `net.c`); a
GlazeBoot boot swaps it in for the CF card (`EMU_ARGS="--cf-eject-after 40
--wl54cf --net-insert-after 45" tests/run_ceboot.sh 85 WL54CF=1`). The emulator's station implementation
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
