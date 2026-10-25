# Physical display controls

Settings > Display controls the BE-300's physical backlight and panel
contrast and contains Wallpaper and Screen Timeout. SpringBoard owns the
settings, loads them at boot, previews slider changes and commits them on
release. Home, Power, deactivation and process
exit restore a pending drag's saved levels. Failed writes keep the previous
preference through a temporary file and atomic rename. Restore Defaults
removes the preference and applies brightness 3, contrast 5.

Screen Timeout offers 30 seconds, 1 minute (default), 2 minutes, 5 minutes,
10 minutes and Never. The separate `display.timeout` preference stores an
int32 number of seconds, with zero meaning Never; missing, malformed and
unsupported values use 60 seconds. Restore Defaults continues to reset only
brightness and contrast. A successful timeout selection notifies SpringBoard
with `SB_NOTIFY_DISPLAY`, immediately reloading and restarting the countdown.

After inactivity, SpringBoard cancels held gestures and adjustment previews,
closes overlays, locks, blanks the framebuffer and disables the backlight.
The timeout also applies on the lock screen and during media playback.
Apps, audio, input scanning and notifications continue running; rendering
and notifications neither restart the countdown nor wake the display.
Touch, a button press or keyboard text wakes to the lock screen, whose full
frame is drawn before the backlight is restored. The waking touch/button
sequence is consumed through release, including button repeats. A text-only
keyboard event is consumed individually. Releases of input already held at
timeout do not wake the screen. Unlock restores the foreground app, subject
to the existing passcode check. Manual Power behavior while awake is unchanged.

The kernel exposes `SYS_DISPLAY_SET(brightness, contrast)` to privileged
processes. Apps use the foreground-Settings-only `SB_DISPLAY_*` protocol;
they cannot access display registers directly. Existing syscall and protocol
numbers stay unchanged. The preference `sb.display` stores a versioned pair
of native levels. Invalid or missing records use the defaults.
`SYS_DISPLAY_POWER(on)` is also privileged and accepts only 0 or 1. It
changes backlight power without suspending the CPU or disabling touch.

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
- The mapping constructor at `0x01A54614`–`0x01A546B4` assigns object offset
  `0x44` to companion `0x0980` (BLG) and object offset `0x5C` to `0x1128`
  (CMM). The backlight routine at `0x01A55900`–`0x01A55990` takes a nonzero
  argument for **off**: write zero to BLG+0 and BLG+4, then clear CMM bit 0.
  Its on branch enables CMM first, configures BLG+4, period BLG+8 and duty
  BLG+12, then enables BLG+0. Glaze uses the stock off sequence and restores
  the saved BLG control/configuration and CMM gate bit on wake, preserving
  unrelated CMM bits. It leaves the inherited period and current brightness
  duty unchanged rather than replacing them with the stock on defaults.

The driver preserves unrelated contrast-register bits and skips unchanged
levels. The existing boot path leaves the LCD and backlight enabled. Timeout
only powers down the backlight PWM; LCD panel timing and CPU power stay on.
Repeated power requests are idempotent, so they cannot overwrite the saved
configuration with the disabled register values.

## Validation

`make -C host display-test settings-test idle-test touch-calibration-test ui-events-test`
checks the physical duty values, serial bit ordering, latch and edge timing,
range validation, persistence, rollback, and Settings rendering and gestures.

The emulator latches these registers but does not model their optical effect.
Emulator tests can verify register writes, UI interaction and persistence;
backlight intensity and contrast need confirmation on a physical BE-300.
Actual backlight shutdown, wake brightness and repeated sleep/wake cycles
also require physical-device validation; emulator black pixels alone do not
establish a power saving.

`tests/run.sh tests/t_display.txt 19` saves new levels and cancels a drag
with Home. Then run
`FS_FROM=out/shots/t_display/glaze.bin tests/run.sh tests/t_display_reboot.txt 12`
to verify the saved levels after reboot. Both runs should end on Display
with brightness 5 and contrast 8.

`tests/t_screen_timeout.txt` selects 30 seconds, then uses two unlock swipes.
Separate runs ending at 41, 44 and 51 seconds must show black, the lock
screen, then Display, respectively. Boot `tests/t_screen_timeout_reboot.txt`
with the saved filesystem and stop at 40 seconds to verify the saved 30-second
timeout. Set `BE300` to an absolute emulator path when running from a worktree.

Additional scripts cover Never (`t_screen_timeout_never.txt`, 75 s), a held
brightness preview (`t_screen_timeout_drag.txt`, 51 s), and a partially
entered passcode (`t_screen_timeout_passcode.txt`, 58 s, boot with the saved
30-second filesystem). These must end on Display with Never, Display with
brightness 3, and the home screen after entering 1234, respectively.
