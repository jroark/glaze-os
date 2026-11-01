# Hard icons

The BE-300's resistive touch panel is taller than its LCD. Below the
240x320 screen it covers a printed strip of seven icons, which Windows CE
called *hard icons*. Glaze turns a tap on one into an action the user picks
in Settings → Icon Buttons: open an app, run a system action, or send a key.
An app in front can also claim icons for itself.

## What Windows CE did

All of this was recovered from the stock NAND (`be300/ce/restore_images/All_nand_300.bin`).

- **Events.** Casio's *Specification for Developers* (BE-300 SDK, Table 6
  "Hard Icons") says the icons work like keys. ICON1 to ICON7 send
  `VK_LWIN` + `VK_F1` to `VK_F7`.
- **Geometry.** touch.dll holds the strip as a table. It sits just after
  touch.dll's `HARDWARE\DEVICEMAP\TOUCH\HardIcon` string (offset 0x4d108c in
  the decompressed NK flat image, `be300/tools/nk_lzss.py`). The table is a
  header `0x11, 0x18, 7` followed by seven records `{id, 3, x_lo, x_hi, VK}`.
  The x values are in CE's calibrated quarter pixels, where 0 to 959 spans
  the 240 columns:

  | Icon | x (¼ px) | Columns | VK |
  | --- | --- | --- | --- |
  | 1 | 0–137 | 0–33 | F1 |
  | 2 | 138–274 | 34–68 | F2 |
  | 3 | 275–411 | 69–102 | F3 |
  | 4 | 412–548 | 103–136 | F4 |
  | 5 | 549–685 | 137–170 | F5 |
  | 6 | 686–822 | 171–205 | F6 |
  | 7 | 823–959 | 206–239 | F7 |

  The meaning of the first two header words (`0x11`, `0x18`) is not known.
  An icon is any calibrated touch below the LCD.
- **Assignments.** COShell's registry key `SOFTWARE\CASIO\COShell\HardIcon\NN`
  holds the values `File` and `VKey`. In the persisted hive (NAND 0x761c7f),
  00 to 06 are Calendar, Contacts, Tasks, notes, Mail, Internet and
  ControlPanel (F1 to F7). Key 07 (`menu.exe`, F12) belongs to the Menu
  button, not the strip.

## Glaze

**Panel.** The kernel scales the panel to 360 rows
(`kernel/drivers/input.c` `PANEL_H`), and the emulator accepts touches down
to y 359 (`be300/src/hw/piu.c`). SpringBoard maps each touch through the
saved calibration (`sb_calibration_point`) before clamping it to the LCD. A
calibration is fitted only on the LCD, so the strip's position is
extrapolated from it, as Windows CE also did.

**Strokes** (`servers/springboard/hardicon.c`):

- A stroke whose DOWN lands at calibrated y ≥ 320 belongs to SpringBoard.
  None of its events reach the app, the status bar or a banner.
- The icon fires on release if the finger is still over the same icon and no
  more than 8 px above the strip (`HARDICON_SLOP`).
- A stroke that starts on the LCD and slides down onto the strip stays with
  its app, clamped to the bottom row, as before.
- Calibration sampling and the calibration test page still get raw touches.
- On the lock screen, the passcode keypad and the unlock animation, a strip
  tap is swallowed and nothing fires.

**Actions** (`glaze/hardicon.h`, `lib/libc/hardicon.c`, fired by
`hardicon_fire` in `servers/springboard/main.c`), with a tap sound:

| Kind | Effect |
| --- | --- |
| `none` | nothing |
| `home` | the Home button, so a double press opens the switcher |
| `switcher` | the multitasking bar, going home first from an app |
| `lock` | lock |
| `sleep` | Goodbye, then the Settings → Battery sleep mode (Suspend if Never) |
| `screenoff` | dark and locked |
| `app bin/x` | open or warm-resume a built-in app |
| `pkg id` | open an installed package wherever it is (`pkg_find`) |
| `key name` | press and release a key in the front app: `esc ok enter up down left right pageup pagedown tab backspace` |

**Preference.** `sb.icons` is text, one line per icon followed by `apps 0|1`.
An unparsable line keeps that icon's default. The defaults follow Windows
CE where Glaze has a matching app:

```
app bin/calendar
none
none
app bin/notes
pkg gmail
app bin/browser
app bin/settings
apps 1
```

Glaze has no Contacts or Tasks app. Mail does nothing until the Gmail
package is installed. Settings writes the preference and notifies
SpringBoard with `SB_NOTIFY_ICONS`.

**Settings → Icon Buttons** lists the seven icons under CE's printed names,
each showing its current assignment. Tapping one opens a picker with three
groups:

- System;
- Apps: the built-ins in this romfs, then installed packages on either volume;
- Keys.

**App Overrides** (`apps`) lets apps claim icons.

**Apps.** `ui_claim_hard_icons(mask)` sets `sb_shared.hard_icons`, a field
appended to the shared page that older apps leave at zero. While the app is
in front and App Overrides is on, icon n with bit n set is sent as
`GZ_KEY_ICON1 + n` (key down, then up) instead of its action. A new launch
clears the mask. `GZ_KEY_ICON1` to `GZ_KEY_ICON7` are 25 to 31, so
`GZ_KEY_LAST` stays below 32, the limit of USB input's held-key bitmap.

## Tests

- `make -C host hardicon-test` checks:
  - every column's zone against the touch.dll table;
  - preference round trips and bad lines;
  - the stroke rules.
- `make -C host settings-test` renders the Icon Buttons pages and picker
  against full redraws, assigns a system action, a package and a key, and
  checks what gets saved.
- `tests/run.sh tests/t_hardicon.txt 32` runs in the emulator with scripted
  taps at y 340. It opens Calendar, then Notes, checks that a locked tap does
  nothing, opens Settings from icon 7, reassigns icon 2 to Home there, and
  then uses it.
- Real touch: in the emulator window, click the icon strip drawn below the
  LCD (`be300/src/ui.c` `frame_icon_rect`), or use `tests/touch.sh`. On
  hardware, check that the extrapolated calibration really puts the strip at
  y ≥ 320.
