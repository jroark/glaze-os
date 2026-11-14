# MARGI Presenter-to-Go CompactFlash

Glaze has an initial native driver for the MARGI Presenter-to-Go CF VGA card.
It mirrors the LCD as a centered 240x320 RGB565 rectangle on a black 640x480
VGA display at approximately 60 Hz. Insert the card before or after boot;
`drivers/presenter.ko` loads automatically. No Windows CE driver is run.

This implementation has passed native tests against a banked MQ200 I/O model
and a MIPS cross-build. **Physical VGA output has not been validated.** The
BE-300 emulator has no Presenter-to-Go card model. Treat real-card use as
experimental until the hardware checks below pass. This does not implement
MARGI presentation/PDB files, the IR remote, EDID, selectable resolutions,
rotation or scaling. The mirror refreshes in bounded strips; it is intended
for static screens rather than video and can tear during updates.

## Evidence

The user-supplied Dell R57791.EXE contains an InstallShield archive and
`presentertogo.MIPS_PPC.CAB`. The package's two relevant MIPS CE modules are
`ptgload.dll` (`0PTGLOAD.001`) and `PresentertoGo.dll` (`PRESEN~1.002`), image
base 0x100000. Files were inspected outside the checkout; neither binaries
nor recovered vendor materials are included in this repository.

* Loader 0x101378, particularly 0x10149c-0x1014c0: MANFID manufacturer 01d8,
  product masked with fff0 equals 2000. This distinguishes the display card
  from storage, serial and network cards. The registry identity string is
  `MARGI_SYSTEMS-Presenter-to-Go-0014`.
* Loader 0x101b64: enable indirect I/O with 83h at I/O offset 402h.
* Display 0x102520: write card address bits 23:8 to 412h; access the window
  at address & 3feh. 0x102600 performs two dummy halfword reads then samples
  low/high halves; 0x102650 writes low/high halves. This permits access to
  all MQ200 registers and framebuffer through the 2 KiB I/O aperture.
* Display 0x10236c-0x102408: PCI signature at card address 616000h. Glaze
  supports the little-endian layout 02004d51h, not the alternate layouts
  the vendor driver also checks.
* Display 0x1035c0, 0x1035fc, 0x1036c4: CPU interface reset, oscillator,
  power sequencing, and PCI PM state. 0x105e9c-0x105f64 initializes SDRAM
  with explicit 10/5/10/10 ms waits. Glaze preserves those waits using
  deferred service-worker phases.
* Display 0x1048a0 and 0x1049f8: GC1 timing/window registers, PLL2 clock,
  window/scanout enable. 0x104844 maps 16 bpp to direct-color depth c0h.

The register map, RGB565 direct-color fields, PLL formula and standard
640x480 timing were cross-checked against NetBSD's
[MQ200 register definitions](https://github.com/NetBSD/src/blob/trunk/sys/arch/hpcmips/dev/mq200reg.h)
and [MQ200 setup code](https://github.com/NetBSD/src/blob/trunk/sys/arch/hpcmips/dev/mq200subr.c).
PLL2 uses M=211, N=15, P=3 with a 14.318182 MHz reference, giving 25.173864 MHz.

## Lifecycle

Only matching 01d8:200x cards are claimed. Their CIS must provide a valid COR
and a 16-bit configuration with a zero-based 2 KiB I/O range (or at least 11 decoded
address lines). Unsupported configurations fail closed. The dispatcher reserves
the card even if loading or initialization fails: it never sends ATA commands
or passes it to GPS/Bluetooth. No new user-accessible MMIO syscall is added.

Module loading and transfers run in the service worker, not a timer interrupt.
Each pass transfers at most eight scanlines and checks a 2 ms budget between
lines. The VGA surface is cleared before scanout is enabled. The mirror reads
only the 240 visible LCD pixels from each 256-pixel source row. Removal unloads
the module without further card I/O; sleep cuts the card's power, and resume
reinitializes it. No card image, firmware, credentials or external service is
required.

## Validation

```
make -C host presenter-test presenter-lifecycle-test cf-boot-test service-test
# GCC14 currently exposes an unrelated unresolved trunc in apps/wince.
# Exclude that optional application from the selected development build.
dirs=$(find apps servers -mindepth 1 -maxdepth 1 -type d | \
  grep -Ev '^apps/(wince|ktest|ktest_pong|ldtest.*|ldmem)$' | tr '\n' ' ')
make -j4 WL54CF=0 PROG_DIRS="$dirs" NAND_DROP=wince
```

`presenter-test` checks actual driver startup, bank crossings, the full RGB565
frame and black borders, invalid configurations, a wrong MQ200 signature,
reinsertion and removal without I/O. `presenter-lifecycle-test` checks deferred
module loading, descriptor validation and failure cleanup. `cf-boot-test`
checks identity matching and rejects ATA probing of the card.

On hardware, connect VGA before insertion, boot a copy of the candidate NAND
or use the normal GlazeBoot RAM workflow, then check the `presenter:` log.
Confirm a monitor locks at 640x480/60, colors and all four screen edges match,
and touch/audio remain responsive. Exercise removal/reinsertion and suspend/
resume, then replace it with a storage, GPS or network card to check dispatch.
Record the full CIS and monitor result before widening supported configurations.
