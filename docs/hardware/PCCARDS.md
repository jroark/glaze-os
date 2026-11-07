# PC Cards through a PCMCIA jacket

A PCMCIA jacket with its own supply plugs into the BE-300's CF socket and
takes 16-bit PC Cards. Its cards are on a second socket controller, which
Windows CE calls socket 1 (see "Socket 1" below). Glaze has drivers for
three kinds of card, all working on a real BE-300 through the jacket
(2026-11-05). The 3c589 and Hermes modules are in romfs with `PCCARDS=1`,
the default for GlazeBoot builds; a NAND image needs `PCCARDS=1
NAND_DROP=wince` for room. Dial-up needs `MODEM=1` likewise. On a stock
image, install the signed `hw-pccards` and `hw-modem` packages instead
([hardware packages](../os/HARDWARE_PACKAGES.md)): a card already in the
jacket comes up as soon as its driver is installed.

| Card | CIS identity | Driver | Emulator |
| --- | --- | --- | --- |
| 3Com EtherLink III (3c589) | MANFID 0101:0589, FUNCID 6 | `drivers/3c589.ko` (`kernel/drivers/3c589.c`) | `--3c589` |
| Lucent/Agere WaveLAN/IEEE, Orinoco (Hermes) | MANFID 0156:0002, FUNCID 6 | `drivers/hermes.ko` (`xi825.c` with `XI825_HERMES`) | `--hermes` |
| Modems | FUNCID 2 with a modem FUNCE | `drivers/cfuart.ko` through `kernel/modem.c` ([MODEM.md](MODEM.md)) | `--cf-modem` (a CF modem) |

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
The emulator's `--hermes` puts its XI-825 model in an Agere firmware mode,
with a real Orinoco's CIS; `--hermes-firmware 6.4` models the older
firmware (no 0xFCB2, scan results only for an empty desired SSID).
With that firmware the driver turns the port off again once a scan it
enabled the port for is over, so the card doesn't join an open network
by itself, and an empty scan frame completes the scan.

On a real BE-300: an Orinoco Gold (station firmware 8.72.1) and a WaveLAN
Gold (6.4.1), both VERS_1 "Lucent Technologies" "WaveLAN/IEEE", identify,
read their MAC and scan (4 and 7 networks). Joining a network is not yet
tested on hardware: it needs an open or WEP network.

## Modems and dial-up

A modem in the jacket uses the CF modem support ([MODEM.md](MODEM.md)):
`capture_card` gives a serial card whose FUNCE names a modem, with an entry
decoding COM1, to `kernel/modem.c`, which loads `drivers/cfuart.ko`. The
UART module writes the COR and finds its registers through the socket's
windows (`pcmcia_attr_base`, `pcmcia_io_base`), so the same path serves
both sockets. The network server's Hayes dialer and PPP, and Settings →
Dial-up, are unchanged.

On a real BE-300, a USR/Megahertz XJ/CC1560 56k modem (MANFID 0102:002F,
VERS_1 "U.S. Robotics" "XJ/CC1560 Megahertz 56kbps Modem", CFTABLE entries
for the four COM ranges, CONFIG at 0x300) was found and answered the
dialer with `NO DIALTONE` with no line, using an earlier driver on this
branch; a PPP call over a real line is not yet tested.

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
| 0x0A001B50 bit 3 | the socket's interrupt (GIRQ0 source 6) | | |
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

The emulator models socket 1 with `--jacket` (be300 `src/hw/jacket.c`):
the controller, the detect bits, the interrupt (source 6), per-card status and both
windows, from these captures. Test the cold power-up path with

```sh
./build.sh INIT=bin/ktest NAND_DROP=wince PCCARDS=1
be300 --nand out/glaze.bin --rtc-host-time --jacket --3c589   # or --ne2000, --hermes
```

### Socket 1's interrupt

Traced on a BE-300 with a 3c589 interrupting (VeoTest, `glaze-usb peek`
at 0x0B000000) and in nk.exe's OAL (handler 0x800AB2A4/0x800AB394, enables
0x8007B088 and 0x800A9420):

```
card IREQ -> 0x0B00053C bit 0   socket 1's cause (read clears; 0x22: card change)
          -> 0x0B000200 bit 1   the controller's summary
             (0x0B000208 = 1)   its enable
          -> 0x0A001B50 bit 3   companion jacket events (mask 0x0A001B58 = 8;
                                bits 0-2 other events, acknowledged at 0x0A001B54)
          -> GIRQ0 source 6     enable 0x0A000028
```

0x0A001B50 bit 2 reads set all the time, so the mask must be 8 before the
source is enabled, as the OAL sets it. Source 2 (cause 0x0A001040) is a
different path: pin detectors at 0x0A001044 (bit 0), 0x0A00104C (bit 2,
the jacket's card detect) and 0x0A001054 (bit 4), each cleared with 0x30
and armed with 3 (bit 6 is the level).

Glaze arms source 6 from the tick once a jacket card's driver runs
(`pcmcia1_irq_arm`); `irq.c` masks it while serving the card, clears the
cause (`pcmcia1_irq_ack`) and enables it again. With the tick's polling
of socket 1 turned off, a 3c589 took a DHCP lease on interrupts alone.
The tick still polls as a backup, as for the CF socket.

ATA cards in the jacket aren't supported (only the CF socket's ATA ports
are used).

Don't read 0x0A00D000 from Glaze with only a jacket card in: a HWDUMP
`peek` there crashed Glaze, and the device restarted into CE. `glaze-usb
peek` and `poke` reach socket 1's controller and windows at 0x0B000000 on
HWDUMP=1 kernels; the controller reads 0 while its bus is off.
