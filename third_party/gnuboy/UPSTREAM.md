# gnuboy in Glaze

Source: https://github.com/rofl0r/gnuboy
Commit: `4c2f6c441eb3b364e3c8e211ec39241718d5e189`.

Copyright (C) 2000-2001 Laguna and Gilgamesh, with contributions from other
authors. GPL-2.0-or-later: upstream `main.c`'s `copyright()` explicitly permits
version 2 or any later version. `COPYING` is the upstream GPLv2 text; `NOTICE`
records that election. The combined Glaze package is distributed under GPLv3.

Only CPU, memory, hardware, scanline LCD and integer APU code and their headers
are vendored. Glaze's `packages/gameboy` adapter replaces the upstream loader,
configuration frontend, frame loop, audio/video backends and RTC persistence.
There is no SDL, POSIX file API, floating point, assembly or boot ROM dependency.

Local changes:

- Page-local memory-map pointers and explicit 16-bit bus wrapping replace pointers
  before backing allocations and potentially unaligned word loads/stores.
- MBC1 retains independent low/high/mode registers and maps the fixed bank in RAM
  mode; MBC2 implements address-bit decoding, bank-zero remapping, mirrored
  512-nibble RAM and high read bits; MBC5 permits bank zero and masks rumble RAM
  banks. Absent/2 KiB RAM and invalid MBC3 selectors do not index unallocated RAM.
- CGB DMA destinations wrap within VRAM. The adapter tests GDMA, HBlank DMA and OAM.
- HALT waits with IME clear, wakes on enabled pending interrupts and models the
  duplicated-fetch HALT bug. Interrupt dispatch consumes five machine cycles.
  Invalid opcodes report a bounded error and stop the adapter's frame loop.
- Serial transfers call a platform callback for optional external test oracles;
  the app callback discards output and no link connection is implemented.
- The scanline output backend is RGB565 at 160x144. DMG palettes are grayscale;
  unused scaling/indexed-palette backends are removed. Window scan buffers have
  real left-padding for negative WX. Pattern decoding indexes each VRAM bank
  within its own allocation; CGB LCDC bit 0 controls background priority. Palette packing uses unsigned shifts.
- Signed sound shifts become multiplication; fixed-point PCM is converted from
  unsigned 8-bit to filtered signed 16-bit mono in the adapter.
- Warning-only declaration/indentation and trailing-whitespace cleanup and removal of unused debug hooks.

`rtc.c` is a new Glaze implementation: correct 9-bit MBC3 day registers, 512-day
rollover, sticky carry, halt and latched reads. Wall-clock advancement is arithmetic
and independent of frame speed. Versioned, checksummed metadata restores elapsed
wall time without iterating through missed seconds.

The core remains approximate, with scanline PPU and sound timing. Passing the
Blargg instruction and instruction-timing tests is not a claim of full hardware
accuracy. Serial/link, SGB, infrared and cartridge-specific accessories are not
implemented; unsupported cartridge types are rejected by Glaze's loader.
