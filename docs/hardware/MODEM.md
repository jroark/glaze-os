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
dial-up isn't included in the build, unless the signed `hw-modem` package is
installed: it supplies a PPP `bin/net` that the service worker starts in place
of the romfs one ([hardware packages](../os/HARDWARE_PACKAGES.md)).

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
`E0`.

**This card has never placed a call.** On a phone line (2026-11-09) every
`ATDT102` answered `OK` at once, and the far end never rang. That included tone
and pulse, blind dialing (`X0`), `AT&F` first, and `ATH1` first. It also
printed nothing for `ATI0`, `ATS7?`, `AT+FCLASS?` or `AT&V`, but `ATI3`
answered. So the earlier "`ATDT,` answers `OK`" result shows the same fault,
not a safe test. With the dongle reseated and the 250 ms pause added
(below), it still never took the line off-hook: the simulator's line LED
stayed dark and the router saw no ring. Its line side is faulty.

### USR XJ/CC1560 in the PCMCIA jacket: a real call

A U.S. Robotics XJ/CC1560 Megahertz 56k modem in the jacket's socket 1
([PCCARDS.md](PCCARDS.md)) dialed a DCB IP-6690 router over a Teltone TLS-3B
line simulator on 2026-11-09. The router answered with PAP dial-in.

- **The card:** COR `60h` at `0x300`, and the 16550 at 3f8h. It answered
  `AT` at divisor 1 on some boots and at divisor 2 on others; the dialer
  worked at either. `ATI3` answers `TB201, V.90 Modem` / `Regulatory
  revision 1.1`.
- **A pause before each command:** it drops a command sent right after its
  last result code, so the echo and reply never come. The dialer waits
  `HAYES_GAP_MS` (250 ms) before each command. The RC56LD didn't need this.
- **The call:** about 40 s from `ATDT` to `CONNECT
  21600/ARQ/V34/LAPM/V42BIS` on the simulator, then PPP up 0.5–6.5 s later.
  - The router's log shows LCP (asyncmap 0, pcomp, accomp), then PAP. Glaze
    rejects CCP with a Protocol-Reject. IPCP agrees the address and both
    ms-dns entries (and, before VJ was dropped, VJ).
- **Checked on the call (the scratch test app and Settings):**
  - A wrong password is refused (`NET_DIAL_E_AUTH`).
  - Pings to the peer and through the router's NAT (to 8.8.8.8) take about
    165–185 ms.
  - DNS through the router's proxy works, and an HTTP GET of example.com
    returned 200 OK.
  - Hanging up returns the router to dial-in wait. Redialing with the saved
    settings works, and so do Settings' Connect and Hang Up ("Connected,
    21.6 kbps").
- **Downloads:** httpbin bodies of 500 bytes to 30 KB, text and random
  binary, all arrive complete. Binary runs at about 18 kbit/s, text at
  33–39 kbit/s (the modem's V.42bis). The Browser loads 68k.news (97 KB).
  Three bugs stalled anything over about 1 KB until they were fixed:
  - **PPP input dropped under load.** `pppos_input_tcpip` copies each read
    (often a few bytes) into a pool pbuf for the tcpip thread's mailbox, and
    drops the bytes when either is full (`ERR_MEM` from the first frames
    after PPP came up). The server now calls `pppos_input` under the core
    lock.
  - **Van Jacobson compression corrupted IP headers.** With pppd's VJ, most
    TCP segments rebuilt by lwIP's `vj.c` failed the IP header checksum
    (`ip.chkerr` 15 of 24, `link.chkerr` 0), so TCP stalled at 8,760 bytes.
    `VJ_SUPPORT` is now 0 (`servers/net/lwipopts.h`). Not traced further
    into `vj.c`.
  - **The receive ring overflowed.** While the Browser was busy, a 97 KB
    download filled cfuart's 4 KB ring at 115200. The driver faulted, and the
    modem then didn't answer `ATZ` either. Modems now use RTS/CTS flow control
    (`uart_config.rts_cts`, and the USR is set `&H1&R2`): RTS drops from 3/4
    full until the ring drains to 1/4, and nothing is sent while CTS is low.
    A dial first resets a faulted UART.
- **Open: the Browser faults on a big page with little memory.** With the
  hwtest RAM image (`ramfs_kb=6656`, about 2.4 MB free), opening 68k.news
  ended in `fault: TLB load in browser` (a code page it couldn't map). With
  `ramfs_kb=2048` (7.4 MB free) the page loads.
- **Open: freezes.** Twice the device froze with the screen on and USB
  not answering, and needed a reset. Once was after about 25 minutes of
  calls; the console before it shows nothing unusual. Once was while
  booting a USB RAM reload; the same image booted normally next time.
- **Router configuration:** the router's first DNS server pointed at a dead
  address. Its proxy answered names it hadn't cached too slowly, and
  `net_resolve` failed with `GZ_ENOENT`.

PPP is also tested in the emulator, as described below.

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
- No Van Jacobson compression (`VJ_SUPPORT` 0): see the downloads above.
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
