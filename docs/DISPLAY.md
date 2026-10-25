# Physical display controls

Settings > Display controls the BE-300's physical backlight and panel
contrast. SpringBoard owns the settings, loads them at boot, previews slider
changes and commits them on release. Home, Power, deactivation and process
exit restore a pending drag's saved levels. Failed writes keep the previous
preference through a temporary file and atomic rename. Restore Defaults
removes the preference and applies brightness 3, contrast 5.

The kernel exposes `SYS_DISPLAY_SET(brightness, contrast)` to privileged
processes. Apps use the foreground-Settings-only `SB_DISPLAY_*` protocol;
they cannot access display registers directly. Existing syscall and protocol
numbers stay unchanged. The preference `sb.display` stores a versioned pair
of native levels. Invalid or missing records use the defaults.

## Hardware evidence

The register addresses, tables and wire sequence were traced from the stock
BE-300 Windows CE 3.0 NAND image, using `be300/tools/extract_nk_lzss.py` and
`extract_xip_modules.py`. Addresses below are preferred virtual addresses in
`ddi.dll`, whose base is `0x01A50000`.

- `0x01A581B4` maps the companion at physical `0x0A000000`.
- `0x01A56BB0` and `0x01A56BD0` write backlight duty to companion offset
  `0x098C`. Tables at `0x01A52050` and `0x01A52070` both contain
  `0xD0B, 0xD0B, 0x9F9, 0x7AC, 0x3D5, 0`. The stock handler restricts
  battery levels to 0–5. Glaze exposes the five distinct levels 1–5;
  a duty word of zero is maximum brightness.
- `0x01A56A5C` sends an eight-bit contrast command through companion offset
  `0x0C4C`: clear bits 0–2, set data on bit 2, raise clock on bit 1, repeat
  MSB first; then clear bits 0–2, pulse latch bit 0 and leave it low.
  Every edge uses the driver's short delay (`0x01A56930`, argument 5).
  Glaze waits at least five microseconds using its calibrated clock.
- The nominal contrast table at `0x01A63740` is
  `208, 205, 202, 199, 196, 193, 190, 187, 184, 181` for levels 0–9.
  It resides in compressed writable data, not at its raw packed-file offset.
  The table was recovered by running the stock kernel's ROM decompressor at
  `0x800A4BF4` on ddi.dll's data section under a temporary Unicorn harness.
- The stock driver can shift the contrast table using an optional EEPROM
  trim (`0x01A58068`–`0x01A580F0`). Glaze currently uses the nominal table;
  it does not read or modify EEPROM. Defaults are nominal levels, not a
  per-device factory trim.

The driver preserves unrelated contrast-register bits and skips unchanged
levels. The existing boot path leaves the LCD and backlight enabled; this
change does not alter power sequencing, PWM period, or framebuffer pixels.

## Validation

`make -C host display-test settings-test touch-calibration-test ui-events-test`
checks the physical duty values, serial bit ordering, latch and edge timing,
range validation, persistence, rollback, and Settings rendering and gestures.

The emulator latches these registers but does not model their optical effect.
Emulator tests can verify register writes, UI interaction and persistence;
backlight intensity and contrast need confirmation on a physical BE-300.

`tests/run.sh tests/t_display.txt 19` saves new levels and cancels a drag
with Home. Then run
`FS_FROM=out/shots/t_display/glaze.bin tests/run.sh tests/t_display_reboot.txt 12`
to verify the saved levels after reboot. Both runs should end on Display
with brightness 5 and contrast 8.
