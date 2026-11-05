# PC Cards through a PCMCIA jacket

A PCMCIA jacket with its own supply plugs into the BE-300's CF socket and
takes 16-bit PC Cards. Its cards are on a second socket controller, which
Windows CE calls socket 1 (see "Socket 1" below). Glaze has drivers for
three kinds of card, all working on a real BE-300 through the jacket
(2026-11-05):

| Card | CIS identity | Driver | Emulator |
| --- | --- | --- | --- |
| 3Com EtherLink III (3c589) | MANFID 0101:0589, FUNCID 6 | `drivers/3c589.ko` (`kernel/drivers/3c589.c`) | `--3c589` |
| Lucent/Agere WaveLAN/IEEE, Orinoco (Hermes) | MANFID 0156:0002, FUNCID 6 | `drivers/hermes.ko` (`xi825.c` with `XI825_HERMES`) | `--hermes` |
| Modems and other serial cards | FUNCID 2 (not the Bluetooth card) | `drivers/modem.ko` (`kernel/drivers/modem.c`) | `--modem HOST:PORT` |

Other network cards from 3Com, Xircom, Megahertz and Lucent are not NE2000
clones: `net.c` reserves the socket for them and logs `net: unsupported
network card`. Every other FUNCID 6 card still goes to the NE2000 driver,
which checks the DP8390 and its station address before claiming it.

For any card that isn't storage, `cf.c` logs the CIS bytes and then a
decoded summary: each CFTABLE entry's I/O ranges, address lines and IRQ, and
the card's MANFID, FUNCID, CONFIG base and VERS_1 strings. One boot log
identifies a new card.

## 3c589

Register windows over 16 ports (3Com's EtherLink III technical reference,
Linux `3c589_cs.c`). The station address comes from EEPROM words 0-2, high
byte first. The media is 10BASE-T (window 4 link beat and jabber guard).
Frames move by PIO through the window 1 FIFOs, 16 bits at a time; a
transmitted frame is padded to a dword. The card decodes four address lines,
so `cf.c` uses port 0x300 as for the NE2000. On the CF socket it checks
which A020 setting gives 16-bit cycles with a write to the window 2 station
address.

A real 3C589D (VERS_1 "3Com Corporation" "3C589D" "TP/BNC LAN Card Ver.
2a") has its CONFIG registers at attribute address 0x10000, which only
socket 1's window reaches. It took a DHCP lease on 10BASE-T.

## Hermes (Orinoco, WaveLAN Gold)

The same HFA384x-family host interface as the XI-825's PRISM, so `hermes.ko`
is `xi825.c` built again with `XI825_HERMES`. The Agere differences follow
Linux orinoco (`hermes_rid.h`, `hw.c`, `scan.c`):

- scan: on firmware 8.10 and later (host scan), `CNFSCANSSID_AGERE`
  (0xFCB2) left empty, then the scan inquiry; older firmware refuses that
  RID and lists only what it saw looking for the desired SSID, so the
  driver enables its port with an empty ("any") SSID. Results are 50-byte
  records with no header: channel, noise, level, BSSID, beacon interval,
  capabilities, SSID (length + 32 bytes), no rates. The firmware repeats
  networks; the driver drops duplicate BSSIDs.
- current TX rate (0xFD44) in Mb/s, 6 meaning 5.5.
- WEP: four `{length, 14 bytes}` keys in `CNFWEPKEYS_AGERE` (0xFC25), the TX
  key in 0xFCB1, authentication in 0xFC21, enable in 0xFC20.

Open and WEP networks only, as for the XI-825 (see [WIFI.md](WIFI.md)).
The emulator's `--hermes` puts its XI-825 model in an Agere firmware mode.

On a real BE-300: an Orinoco Gold (station firmware 8.72.1) and a WaveLAN
Gold (6.4.1), both VERS_1 "Lucent Technologies" "WaveLAN/IEEE", identify,
read their MAC and scan (4 and 7 networks). Joining a network is not yet
tested on hardware: it needs an open or WEP network.

## Modems and dial-up

`cf.c` picks the CFTABLE entry whose I/O range is a COM port (0x3F8, 0x2F8,
0x3E8, 0x2E8), else one decoding only address lines (placed at 0x3F8). It
writes the COR (level interrupts) and loads `modem.ko` as network kind
`GZ_NET_MODEM` (`pcmcia_modem_config`). The driver is a 16550A at 57600
baud, 8N1, with FIFOs. The PCMCIA interrupt and the 100 Hz tick drain its
receive FIFO into an 8 KiB ring, and RTS drops while the ring is nearly full.
SYS_NET_SEND/RECV carry its byte stream, and SYS_WIFI_CTL takes
`MODEM_STATUS`, `MODEM_DTR` and `MODEM_BAUD` (`glaze/modem.h`), for
`bin/net` only.

The network server (`servers/net/modem.c`) dials with AT commands: `ATZ`,
the init string (default `ATE0V1&C1&D2`), then `ATDT`/`ATDP` and the number.
It reads V.250 result codes. After `CONNECT` it runs lwIP's PPPoS with PAP or
CHAP-MD5 and takes the peer's address and DNS servers. It hangs up by
dropping DTR, and treats DCD loss as a dropped line. Settings → Dial-up
edits the number, user name and password (`glaze/dialup.h`, saved in
`/Library/Preferences/net.dialup`) and connects. With a saved configuration
the server redials when the modem appears and after a dropped line, backing
off from 2 to 30 s, but not after a refused login or "no dial tone".

Test without a phone line through the emulator and `tests/ppp_peer.py`, a
small PPP server (PAP or CHAP, 10.64.0.2 for the guest, ping and DNS
answers):

```sh
python3 tests/ppp_peer.py --port 5555 [--auth chap] &
./build.sh INIT=bin/ktest NAND_DROP=wince
be300 --nand out/glaze.bin --rtc-host-time --modem 127.0.0.1:5555
```

ktest dials `5551212` as `glaze`/`secret` and runs its network cases over
PPP. `tests/ui/t_dialup.txt` drives Settings → Dial-up the same way.

On a real BE-300, a USR/Megahertz XJ/CC1560 56k modem (MANFID 0102:002F,
CFTABLE entries for the four COM ranges) is configured at 0x3F8, passes the
16550A check, and answers the dialer: with no phone line it reports `NO
DIALTONE`. A PPP connection over a real line is not yet tested.

## Socket 1

Glaze's CF socket (CE's socket 0) is the block at 0x0A00A000, with card
detect in A008 bits 1-2 and the C000/D000 windows. The jacket's cards are
on socket 1, a second controller that pcmcia.dll (be300-modules
`74_pcmcia.dll`, base 0x01980000) maps in its table at 0x0198AA64. Found on
a BE-300 with an AmbiCom AMB8610 NE2000 that CE drove (its
`Drivers\Active` key has `Sckt = 1`), using `VeoTest pcmcia`, `snap`,
`snap1` and `s1probe` (`wince/veo_test`, read only):

| Register | Meaning | Card up | Card out |
| --- | --- | --- | --- |
| 0x0A001008 bit 3 | socket empty | 0x4 | 0xC |
| 0x0A00104C bit 6 | socket empty | 0x0C | 0x48 |
| 0x0A001B00 bit 0 | controller bus power | 1 | 0 |
| 0x0A001B50 bit 3 | power good | | |
| 0x0B000100, 0x0B000010 | controller identity | 0x641, 0x7402 | 0 |
| 0x0B000500, 504, 508 | CTRL, RESET, STATUS (like A000-A008) | 0xB, 4, 0x2F9 | 0 |
| 0x0B000520, 524 | like A020/A024 (bit 3: configuration window) | 0x78, 0x40 | 0 |
| 0x0B000108-118, 150 | pin edge detectors (bit 6 level, 4-5 events) | 0x43, 3 | 0 |

The controller reads 0 while its bus is off. Card windows (pcmcia.dll's
window table, 0x0198B9AC): I/O at 0x0B200000 + port, attribute memory at
0x0B600000 (CIS bytes at even addresses, no NAND port in the way).

`cf.c` looks for a card in socket 1 first, then the CF socket, and points
`pcmcia_io_base` and `pcmcia_attr_base` (hw.h; drivers resolve them as
module imports) at the socket that has it. At a RAM boot it takes over a
card CE left powered and configured. Otherwise `socket1_power_up` follows
pcmcia.dll's sequence: bus power (GPIO 5, 0x0A001B00 bit 0, wait for power
good), controller start-up and initialisation (0x0198B46C, 0x0198CDDC),
then the card's supply and reset in the CF socket's order (CTRL 8, 9, 0xB)
and the configuration window; the COR is then written in socket 1's
attribute window. Removing a card cuts its supply, so the next card is
powered and reset afresh. pcmcia.dll's own control and reset helpers are
never called directly, so that card order comes from the CF socket's trace
and the values CE left.

Interrupts from socket 1 aren't routed yet: its drivers run from the 100 Hz
tick. ATA cards in the jacket aren't supported (only the CF socket's ATA
ports are used).

Don't read 0x0A00D000 from Glaze with only a jacket card in: a HWDUMP
`peek` there crashed Glaze, and the device restarted into CE. `glaze-usb
peek` refuses 0x0B000000 addresses.
