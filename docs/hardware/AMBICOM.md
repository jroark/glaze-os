# Ambicom DGCAM-CF (CompactFlash camera)

Status: Glaze mounts the camera's internal storage as `/Card` (2026-11-04, on a BE-300 over USB).
Capture from the PDA isn't supported yet.

## Identity

Read with `glaze-usb bt-info`:

| Tuple | Contents |
| --- | --- |
| VERS_1 `15` | 4.1, `ICSI EMULATION VERSION`, `V`, `000` |
| MANFID `20` | `0000:0000` |
| FUNCID `21` | `04` (fixed disk) |
| CONFIG `1a` | COR at attribute `0x200` |
| CFTABLE `1b` | the four standard CompactFlash ATA configurations (memory, contiguous I/O, primary and secondary IDE) |

IDENTIFY (in 16-bit words):
- Word 0 is `848A` (CompactFlash). The geometry is 250 cylinders, 4 heads and 16 sectors.
- Words 60–61 give 16,000 LBA sectors.
- The model is `ICSI  IC3100PCMCIA`, the firmware is `005 0`, and the vendor area holds `M 3T01087382`.

Sector 0 is an MBR. Its one partition is bootable, type `04` (FAT16 under 32 MB), and covers LBA
16 for 15,984 sectors, so the camera's pictures live on a normal FAT16 volume.

## Board

From photos of the opened card (2026-11-05):

| Ref | Part | What it is |
| --- | --- | --- |
| U2 | ICSI (Integrated Circuit Solution Inc.) IC3102-F128TQ, date code 0242, 128-pin TQFP | camera controller and CF ATA interface |
| U1 | Samsung K9F6408U0C-TCB0 | 64 Mbit (8 MiB) NAND, 512 + 16 byte pages, 16,384 pages |
| — | Vanguard VG36641641DT-7L (labelled `90-114-01000`, `MS41021B1d10`) | 64 Mbit (4M x 16) SDRAM, 8 MiB, probably the frame buffer |
| Y1 | 12.000 MHz crystal | controller clock |
| — | `104` (100 kΩ) resistor arrays by the edge connector | CF bus pull-ups |

There's no separate ROM chip. The image sensor isn't on this side of the board, so the photos
don't show it.

What follows from the parts:
- The chip is an IC3102, but IDENTIFY reports `IC3100PCMCIA`. That string is probably the firmware's
  name for the product family, not the exact part number.
- The NAND holds 16,384 sectors, and 16,000 of them are visible as LBAs. The other 384 sectors
  (192 KiB) are probably spare blocks, and maybe firmware or settings. This explains why reads past
  LBA 16000 fail with IDNF.
- No IC3102 datasheet has turned up. ICSI's launch release (20 June 2002, on the Wayback copy of
  `www.icsi.com.tw/asp/news_data.asp?language=EN&id=82498400F5746C3348256BDE001D0B96`) calls it a
  "CF camera card and CF memory card 2-in-1 controller" for Pocket PC and Pocket PC 2002, sold as a
  "total solution" reference design. Its features: an 8051-compatible ("4 clocks per machine
  cycle") 8-bit CPU, 32 KB of programmable flash ROM, a CMOS sensor interface (up to 500K pixels or
  SVGA), CF 1.4 and True IDE, **full-speed USB 1.1** (for reaching the card "directly without the
  PDA"), SDRAM up to 512 Mbit, NAND up to 128 MB, and **in-system programming through USB or
  PCMCIA**. The firmware is on the chip, so a firmware-update command exists on the PCMCIA side.
- The IC1100-F128TQ card reader chip (January 2002) is from the same family: the same CPU, 32 KB
  flash and 128-pin TQFP, with ISP over USB, PCMCIA or I2C, and a UART on pins 25 and 57. Its
  10-page datasheet (pinout only, no registers) is distributed by Nexty as an ISSI part. Whether the
  IC3102 has the same pinout hasn't been checked. The sibling IC3101 (January 2003) is a USB
  "dual-mode" still camera controller.

## The controller's quirks

The ICSI IC3102 (IDENTIFY says IC3100) needs 16-bit data transfers:
- It aborts SET FEATURES `01h` (8-bit on) and `81h` (8-bit off) with ABRT, so it has no 8-bit
  mode.
- Byte reads of the data register return zeros, and the card stays at status `58` (DRQ) with the
  data unread. 16-bit reads return the data, and status goes to `50` once the sector is read.
- A taskfile soft reset (device control SRST) sent while a data transfer is pending leaves it
  stuck at status `80` (busy), shown by a red LED. Resetting the COR (writing `80`, then `00`,
  then the configuration `43`) recovers it, and the LED turns green again.

`kernel/drivers/cf.c` matches the card by its CIS (`cf_card_is_icsi_camera`). For this card it
moves data in words (`CF_ATA_DATA16`) and never tries 8-bit mode first. Every other card keeps the
byte-wide transfers that real BE-300s need after Windows CE.

## Looking for a capture command

The camera has no shutter button, so the host must trigger a picture somehow. Tried so far,
always reading whole sectors and starting each attempt with a COR reset:
- **Reads past the end:** LBA 16000 and beyond fail cleanly with IDNF (error `10`). There's no
  hidden area after the FAT volume. Sending a new command with a sector still unread hangs the
  card busy, as a soft reset does.
- **Vendor opcodes:** sent 80h–8Fh, 9Ah, C1h–C3h, F0h, F7h, F8h and FAh–FFh with zeroed
  registers and no write data. The security commands, DOWNLOAD MICROCODE, SET MAX, CFA ERASE and
  FORMAT TRACK were left out. Unsupported commands fail with error `20`, not ABRT. Only F0h was
  accepted. It's a data-in command that honours the sector count and fails with IDNF past LBA
  16000 like READ SECTORS, but it returned zeros for every features value (0–15) and every LBA
  tried. One early F0h attempt returned FF. F0h isn't a read of the sector buffer: right after the
  MBR was read, it still returned zeros.
- Now and then every register reads `00` for a moment, even before a command (seen once around
  9Ah and once around 83h, which are otherwise rejected). Something else touching the socket at
  that moment is the likeliest cause.

- **F0h as a write** (LBA 15999, one sector of zeros): accepted, with DRQ, 512 bytes taken, then
  `50` and no error. Flash didn't change, IDENTIFY and the MBR were intact, and the next F0h read
  at that LBA returned FF instead of zeros.
- **F0h with count 0 at LBA 0:** it streamed 256 sectors, all zeros, the same as a READ with count
  0. A second run read past the end and left the card busy (`D0`). After the COR reset, **LBA 0 (the
  MBR) read as erased FF**. LBAs 1–15 and the FAT volume were unchanged. It was restored from the
  backup image with WRITE SECTORS and verified.

**F0h isn't safe.** Some form of it, perhaps combined with the hang, erases or remaps the addressed
sector. Don't send it to a card without a full image backup (`glaze-usb cf-read 0 16000`). It's
probably a vendor flash command, not the camera interface. A guess, from the board: F0h may work on
raw NAND pages rather than the LBA map, which would fit it erasing the MBR's page.

DOWNLOAD MICROCODE (92h) stays off-limits. The IC3102's firmware lives in on-chip flash, which can
be reprogrammed over PCMCIA, and 92h or a vendor opcode could overwrite it.

Still untested: the standard commands used in unusual ways (for example, writes to particular
LBAs) and the other non-vendor opcodes.

## Drivers looked at

- No Ambicom driver could be found: on the web, or in the Internet Archive's copy of ambicom.com,
  whose archived downloads are all 1998–2000 modem and Ethernet drivers.
- WinCESoft's CECam 3.04 (ARM only) supports the LifeView FlyCAM-CF 350K and 1.3M, and the "CAMERA
  CARD" CC33CF and CC130CF (CardCAM, through `CCAMAPI.DLL`). Its detect functions match those cards'
  VERS_1 strings, and none matches this card.

## Probing

In `VEO_PROBE=1` builds, `glaze-usb veo-io SCRIPT` runs raw socket I/O and attribute-memory access
for this card, as well as for the Veo. Run `glaze-usb` with no arguments for the script syntax.
