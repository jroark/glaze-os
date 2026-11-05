# Veo Photo Traveler (CompactFlash camera)

Status: identified on hardware, protocol recovered statically. The bridge works on two BE-300s, but
this card's image sensor never answers (see Hardware results). No Glaze driver yet.

## Identity (read on a BE-300, 2026-11-04)

Read through `glaze-usb bt-info` from Glaze's captured CIS:

| Tuple | Contents |
| --- | --- |
| DEVICE `01` | `79 fd ff` |
| MANFID `20` | `ffff:0001` (no registered vendor) |
| VERS_1 `15` | 5.0, `VEO`, `CFTraveler622` |
| FUNCID `21` | `fe` (vendor specific), `01` |
| CONFIG `1a` | last index 1, register base `0xff` |
| CFTABLE_ENTRY `1b` | index 1 (default), I/O and memory interface, one Vcc descriptor, 16-bit I/O only, one range of 16 ports at base 0, no IRQ or memory descriptors |
| DATE `44` | 2002-03-15 |

Windows CE registers the driver under `Drivers\PCMCIA\VEO-CFTraveler622-FE86` (Prefix `CAM`,
`veocam.dll`). Because FUNCID is not fixed disk, `kernel/drivers/cf.c` never sends it ATA commands.

## Sources

Veo's Pocket PC package `travelepke002.exe` (version 2e, 2003-05-06) contains
`veotraveler.mips.CAB`, with `veocam.dll` (34,304 bytes, SHA-256 `79c2d03b…fe942`) and
`VeoTraveler.exe`. These were reverse-engineered with Ghidra 12.0.4 and llvm-objdump. The vendor
files and the analysis outputs stay outside the repository.

## Socket setup in the CE driver

- Card Services from `PCMCIA.DLL`: CardRegisterClient, CardRequestSocketMask, CardRequestWindow
  (attributes I/O space | 16-bit, 10 bytes, access speed 0x80), CardMapWindow (card offset 0,
  10 bytes), and CardRequestIRQ.
- **It never calls CardRequestConfiguration**, so the COR is never written (the CIS's odd `0xff`
  CONFIG base is never used). The card decodes I/O while unconfigured.
- The IRQ handler only signals an event. All transfers are polled with 1 ms sleeps, so Glaze can
  start polled-only.

## Register interface (I/O window base B, all accesses 16-bit)

| Port | Use |
| --- | --- |
| B+0 | Data FIFO. Each 16-bit read returns two stream bytes, high byte first. `0xFFFF` means no data. |
| B+4 | Read port: write `reg << 8`, then read; the value is in the low byte. |
| B+6 | Write port: write `(reg << 8) \| value`. |

Status registers:
- `0x0D`: bit 7 = sensor-bus engine busy, bit 6 = frame aborted or overflowed (restart), bit 5 = data ready.
- `0xE2`: preview health. `(v & 0xCF) == 0xCF` while the card is running; bit 5 = frame in
  progress, bit 4 = skip this frame.

Control writes used around a frame:
- `0x0C`: `0x80` arms, `0x00` stops.
- `0xB3`: `0x00` then `0x03` restarts the pipeline.
- `0xE3`: `0x4F` for preview, `0xFF` for a still.

## Bring-up sequence (driver function order)

1. Bridge reset: `0x00C0`, `0x0080` (reg 0x00), `0xB3:00`, `0x11:00`, then `0x11:(0x88 + n)` where
   n = 1..6 is a clock or divider choice (the driver uses 2), then `0x2C:BC`, `0x2A:01`, `0x2B:D8`.
2. Sensor power: `0x0F:41`, wait 30 ms, `0x0F:61`, then `0x19:B1`, `0x08:00`.
3. Read-modify-write reg `0x19`: set bit 2 for a still, clear it for preview.
4. Write `0xB0:08`, `0xB1:38`, `0xB2:2C`, `0xB3:03`, `0xE1:09`.
5. Probe the sensor over the serial bus: sensor register 0 must read `0x18`, otherwise the camera
   is absent or failed. Then write sensor reg `0x1C` = 0.

## Sensor serial bus

The bridge has a sequencer for a two-wire bus (I²C-like). The sensor's write address is `0xAA`
and its read address is `0xAB`.
- Register `0x1F` (with `0x80` = go) and register `0x20` take a packed list of 3-bit opcodes
  (start, stop, address/data byte, read). Byte operands are loaded into slot registers `0x27`,
  `0x2D`, `0x2E`, `0x2F` and `0x30`, in order.
- Wait for reg `0x0D` bit 7 to clear. A read result is in reg `0x26`.
- The driver encodes "write sensor reg r = v" as start, address `0xAA`, r, v, stop. A read is start,
  `0xAA`, r, restart, `0xAB`, read, stop. Opcode bit patterns and slot order are in the driver's
  sequencer routine; re-derive them from the disassembly when writing the Glaze driver.

## Modes

Bridge window registers `0x90`–`0x99` (pairs of high/low bytes) together with `0x02`–`0x05` and
`0x0A` select the output size. The sensor gets a matching table over the serial bus (registers
2–6, 0x0A–0x0E, 0x19, 0x1B, plus exposure 0x0F–0x15).

| Mode | Output | Use in the CE driver |
| --- | --- | --- |
| 1 | 640×480 | Still only |
| 2 | 320×240 | Still, or preview |
| 3 | 160×120 | Still, or preview (default preview) |

Bridge reg `0xB2` selects one of the card's two fixed quantisation sets: `0x39` "fine" (flat 6)
or `0x38` "normal" (close to Annex K × 0.5). The driver's quality flag 0 selects fine.

## Output: the card encodes JPEG itself

The FIFO carries a baseline JPEG entropy-coded scan, not raw pixels. The driver:
1. Drains the FIFO until it reads `0xFFFF`, then reads words until the next `0xFFFF`, in blocks of
   256 reads (up to 600 blocks for a still, 75 for preview).
2. Accepts the frame only if it ends with an `FF D9` word followed by `00FF`. Preview frames shorter
   than 320 words (or 1024 in mode 2) are discarded.
3. Byte-swaps the words, then saves a fixed 399-byte JPEG header followed by the data as a `.jpg`.

The header is JFIF APP0, DQT (two tables), SOF0, DHT and SOS:
- SOF0: 8-bit, size per mode, three components: Y with H2V1 sampling and Cb, Cr with H1V1 (4:2:2).
  Y uses quant table 0; Cb and Cr use table 1.
- DHT: only the ITU T.81 Annex K luminance DC and AC tables (K.3, K.5), and all three components
  use them (SOS `01:00 02:00 03:00`). `lib/media/jpeg.c` already has these tables.
- DQT: fixed by the card's encoder, so `lib/media/veo_jpeg.c` embeds the measured values. "Normal"
  is close to Annex K × 0.5 but not an exact IJG scaling, and "fine" is a flat 6.

For a still at 640×480, the driver also decodes the frame once as a sanity check and retakes it if
decoding fails.

## Exposure and white balance (done in software by the driver)

After each preview frame, the driver reads brightness statistics from bridge regs `0x80`–`0x82`
and `0x70`–`0x72`, forms a weighted mean, and steps an exposure index (0..107, starting at 70) to
keep the mean within ±8 of a target of 121. It writes the index's sensor values to sensor regs
`0x0F`–`0x15`. With auto white balance on, it switches between two gain sets from the R/B ratio.
Glaze can begin with a fixed mid-table exposure and add this loop afterwards.

## Implications for Glaze

- `cf.c`: match VERS_1 `VEO`/`CFTraveler622` (MANFID `ffff:0001`, FUNCID `fe`). Map a 16-bit I/O
  window and skip COR writes. Check A020's bus width with a write and read-back of a writable
  register (for example `0xB0`), the way `prism_bus_width()` does.
- Kernel: a small `drivers/veocam.ko` that exposes 16-bit register and FIFO I/O to one privileged
  service. A polled FIFO burst of 256 reads per call is enough.
- Service and app: preview at 160×120 or 320×240, a JPEG header built from the parameters above,
  decoding through stb_image (already linked into Photos), and stills saved directly as JPEG.
  No JPEG encoder is needed.

## Hardware results (2026-11-04)

These were run on two BE-300s with this card, using the `VEO_PROBE=1` USB diagnostics
(`glaze-usb veo-*`, `kernel/drivers/veo.c`) and, under stock CE, `wince/veo_test`
(`make veo-test-exe`).

- Writing and reading back slot register `0x27` works at any I/O base (the card aliases its 16
  ports), with 16-bit cycles under both Glaze's socket setup (A020 `40`) and CE's (A020 `70`,
  A024 `50`, A034 `0C`). Attribute memory has no COR: offset `0x200` reads `FF`.
- The bring-up writes land (`11=89`, `2C=BC`, `2B=D8`, `19=B5`), and the sequencer runs (its
  result register changes from `00` to `FF` after the first bus operation).
- The sensor never answers: every sensor register reads `FF`. Waiting up to 10 ms after each
  sequencer run, all six `0x11` dividers, a `0x0F` power cycle and I/O base 0 changed nothing. With
  a mode programmed, status `0x0D` stays at `1E` (never data-ready) and `0xE2` stays at `FF`.
- Veo's own driver cannot run on a BE-300: CE's `pcmcia.dll` rejects its CardRequestWindow
  (16-bit I/O, 10 bytes), and the driver then reports the error and continues with I/O base 0.
  Do not send it IOCTLs in that state.

CE's own socket setup gives the same result as Glaze's, so the likeliest explanation is a failed
sensor on this card. Testing the card on a Pocket PC with Veo's software would confirm that.
Note that Veo's driver ignores the sensor-ID check during bring-up, so a working card might also
read `FF` at that point. The encoder never producing data is the stronger sign.
