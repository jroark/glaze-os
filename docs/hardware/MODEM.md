# CF dial-up modem

Glaze dials out over a CompactFlash data modem. lwIP's PPP runs over the
card's UART, so the existing `glaze/net.h` and BSD sockets work over dial-up
with no change to apps. Settings → Dial-up holds the phone number, user name,
password and an optional init string, and has Connect/Hang Up. The settings
are saved when you connect.

PPP is a build option because romfs has no room for it. `MODEM=1` adds lwIP's
PPP and the Hayes dialer to `bin/net` (36 KiB). GlazeBoot builds
(`make glazeboot`) default to it. NAND images need `MODEM=1 NAND_DROP=wince`,
because the default NAND romfs is within a few KiB of its 0x4C0000 slot.
Without `MODEM=1`, a modem card still gets its kernel driver, and Settings says
dial-up isn't included in the build.

## Verified hardware

P/N CF56R-BCT-DX, product code 136188, D/C 036. The CIS was read on a BE-300
through Glaze USB on 2026-11-04:

- VERS_1 `Rockwell Sem. Sys.`, `RC56LD-PCC`, `021`, `A`. MANFID `0013:0000`,
  FUNCID `02` (serial).
- Serial FUNCEs of types 00 (16550 UART), 05/06/07 (data, fax and voice
  capabilities), 02 (data modem), 13 and 84.
- CONFIG: COR at attribute `0x420`, last index `0x0d`, mask `77`.
- CFTABLE: entry 5 with three address lines (any eight-port base), then index-5
  ranges at 3f8h, 2f8h and 3e8h, and index 0x0d at 2e8h.

Glaze writes COR `45h` (entry 5, level interrupts) with PCMCIA_A020 bit 3 set,
the same path as the network cards' COR. The 16550 is then at 3f8h in the
socket's I/O window (`0x0A00C3F8`). MCR `0bh` sets DTR, RTS and OUT2. Divisor 1
(115200 from the usual 1.8432 MHz clock) with 8N1 answers `AT` → `OK`. `ATI3`
answers `L27-01`.

This is a controller modem, not a host-signal "soft" modem: the AT interpreter
and the data pump are on the card. Unlike most modems, it keeps echoing
commands after `ATE0`, so the dialer recognises echoes instead of relying on
`E0`. `ATDT,` (no digits) goes off-hook and answers `OK` straight away. That
makes it a safe way to exercise the dialer on a real line without calling
anyone.

Checked on hardware through a Glaze USB RAM reload: the AT probe, the Settings
pages, and the dialer's ATZ / setup / ATI3 / ATD / result / DTR hang-up. A real
call and PPP session over a phone line haven't been tested yet. PPP itself is
tested in the emulator, as described below.

## Kernel

`kernel/drivers/cf.c` sorts a FUNCID-2 card as a modem when one of its serial
FUNCEs has type 1, 2 or 5 (modem, data modem, data-modem capabilities) and a
CFTABLE entry can decode 3f8h. The exact-match SMART Bluetooth card is checked
first. The modem is then reserved like the Bluetooth card and never gets ATA or
network probes.

`kernel/modem.c` loads `drivers/cfuart.ko`, the 16550 module it shares with
Bluetooth (`kernel/drivers/cfuart.c`, `struct uart_driver` in
`kernel/module.h`). At insertion it sends `AT` at divisors 1, 2 and 12 and
keeps the first that answers. It logs the reply and `ATI3`.

`SYS_MODEM_IO` (`glaze/modem.h`) is for `PROC_NET` only. Its operations are
RESET, READ, WRITE (at most 256 bytes), LINES (DTR, RTS), STATUS (the MSR's
DCD/CTS/DSR/RI) and BIND. BIND makes the kernel send the net server
`GZ_NOTIFY_NET` when received bytes are waiting.

A modem counts as a network card for services. The `net` service starts with
it, and `gz_services` reports `GZ_NET_MODEM`. Swapping either a modem or a
network card restarts the service.

## Network server

`servers/net/hayes.c` is the dialer, a byte-stream state machine with no I/O
of its own:

1. `\rATZ` (tried three times).
2. `ATE0V1X4&D2&C1`: verbose results, CONNECT rates, hang up on DTR, DCD
   follows the carrier.
3. `ATI3`.
4. The optional init string.
5. `ATDT`/`ATDP` with the number, waiting up to 90 s for `CONNECT`. `BUSY`,
   `NO DIALTONE`, `NO ANSWER`, `NO CARRIER` and `ERROR` become `NET_DIAL_E_*`.

Numbers may only contain digits, `, W * # P T ( ) -` and spaces, and init
strings may not contain `;`. Nothing a user types can end the dial command and
start another (`net_dial_config_validate`).

`servers/net/modem.c` runs the dialer, then lwIP's PPPoS:
- Authentication is PAP or CHAP-MD5, whichever the peer asks for. With no user
  name, none.
- Van Jacobson compression is offered.
- IPCP supplies the address, and its DNS servers are used.
- The PPP interface replaces the Ethernet one (`LWIP_SINGLE_NETIF`), so
  `net_status` reports `NET_ASSOCIATING` while dialing and `NET_UP` online.

Hang-up closes PPP (LCP Terminate) and drops DTR after at most 3 s for 1.5 s.
The next call waits until lwIP reports the session ended. If DCD has been up
and then drops, the carrier is lost.

Dial settings are kept in the preference `net.dialup`, password included, as
Wi-Fi keys are kept. `NET_DIAL_CONFIG` never returns the password.
`keep_password` redials with the saved one.

## Emulator and tests

be300's `--cf-modem <spec>` is an RC56LD model with the CIS above, a 16550 at
3f8h and a Hayes interpreter (`src/hw/modem.c`). A dialed call connects to a
host_chardev spec such as `tcp:127.0.0.1:5640` or `pty:auto` (for a host pppd).
Any number answers `CONNECT 52000`, and dropping DTR gives `NO CARRIER`.

`tests/ppp_peer.py` is a userspace PPP access server. It handles LCP, PAP,
IPCP (10.64.0.2 for Glaze, 10.64.0.1 as router and DNS), ICMP echo and DNS A
answers, and needs no root or kernel PPP. `tests/run_modem.sh` builds a
`MODEM=1` ktest NAND and boots it against the peer. ktest's modem cases cover:
- dial-string injection;
- a wrong PAP password (`NET_DIAL_E_AUTH`);
- dialing while busy;
- IPCP addresses, ping and DNS through the peer;
- saved settings, hang-up, and redialing with the saved password.

Native tests:
- `make -C host modem-chat-test`: the dialer against the card's captured
  replies.
- `make -C host modem-driver-test`: `kernel/modem.c`.
- `cf-boot-test`: the captured CIS.
- `service-test` and `settings-test` (Settings → Dial-up).
- `ctest -R modem` in be300: the model.
