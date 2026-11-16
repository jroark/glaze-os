# Landscape

The BE-300 has no accelerometer, and its 240x320 LCD can't scan out
sideways. Landscape is therefore something the user switches on by hand,
and SpringBoard draws it in software. Apps opt in. As on iPhone OS 1 to 3,
the home screen, lock screen, passcode keypad and switcher stay portrait;
only an app that can draw landscape turns.

## Turning it on

Double-press Home to open the multitasking tray, then swipe the tray right
to show its controls page. As on iPhone OS 4, the page sits to the left of
the running apps; dots at the bottom of the tray show which page is up. Its
one button shows a handset upright ("Portrait") or on its side
("Landscape", highlighted). Tapping it flips the setting, which SpringBoard
keeps as the `sb.landscape` preference across reboots.

Nothing turns on the spot: the tray is on the home screen. A landscape-capable
app takes the new shape the next time it comes up, from the tray or its icon.

## Orientation

There is one landscape orientation. The top of the picture is at the LCD's
left edge, so the user turns the device clockwise and the hard-icon strip
ends up on their left. On the 320x240 landscape screen, point (x, y) is LCD
pixel (y, 319 - x). Touches map back the other way: LCD (px, py) is screen
(319 - py, px).

The strip of printed hard icons below the LCD stays in LCD coordinates
(`docs/os/HARD_ICONS.md`): an icon does the same thing either way round.

## Apps

An app that can lay itself out at 320x224 says so right after `ui_init`,
before it draws:

```c
ui_init(SB_STATUS_GRAY);
ui_set_orientations(SB_ORIENT_PORTRAIT | SB_ORIENT_LANDSCAPE);
```

It then lays out from `ui_w()` and `ui_h()` (or `ui.surf.w`/`h`), never from
`UI_W`/`UI_H`, which stay the 240x304 portrait size for the apps that don't
opt in. `ui_landscape()` tells the two apart. The surface is:

| | Portrait | Landscape |
| --- | --- | --- |
| With the status bar | 240 x 304 | 320 x 224 |
| `SB_STATUS_FULLSCREEN` | 240 x 320 | 320 x 240 |

If the setting changed while the app was away, `ui_next_event` returns
`GZ_EV_APP_ACTIVATE` and then `GZ_EV_APP_RESIZE` (x, y = the new size).
`ui.surf` has already been reshaped by then and is all dirty. Recompute
anything cached from the old size (list areas, scroll extents, text wrapping),
then redraw and present. SpringBoard shows the app only once it has
presented the new shape. An app that passes only `SB_ORIENT_LANDSCAPE` is
always landscape. An app that never calls `ui_set_orientations` is always
portrait, and so is every app on a SpringBoard older than this.

libui's widgets size themselves from the surface they draw into: the nav
bar, bar buttons, tab bar, alerts and push/pop transitions. The keyboard,
its edit panel and the handwriting pad keep their 240x150 layout and scale
it to 320x120 in landscape, so `ui_keyboard_height()` is `UI_KB_LAND_H`
there. Touches are mapped back into that layout, so key hits and ink are
the same as in portrait.

Built-in apps that turn: Calculator, Clock, Notes, Photos, Video and Browser.
The Browser's viewport is 426 CSS px wide in landscape (the same 3/4 device
px per CSS px), and a turned page is restyled, so `vw`, percentages and
`orientation`/`width` media queries follow.

## Protocol

`glaze/sb.h`:

- `SB_SET_ORIENTATIONS` (w1 = `SB_ORIENT_*` mask, w3 = pid). The reply
  carries the surface's orientation, width, height and stride in w1 to w4.
- `struct sb_shared` gains `orientation`, `width` and `height` after
  `hard_icons`. They are appended, so the header's old fields keep their
  offsets. SpringBoard writes them, with `stride`, before HELLO's reply,
  `SB_SET_ORIENTATIONS`' reply and ACTIVATE.
- The surface memory doesn't change size: 240x320 RGB565, either way round.

SpringBoard (`servers/springboard/main.c`):

- `slot_shape()` lays out a slot's surface; `slot_wants_landscape()` decides
  which shape it should have.
- `present_app_rect()` draws a landscape app with `gfx_blit_rot90` (libgfx).
  The status bar and banner are drawn upright into a 320-wide strip and
  turned the same way. The zoom and the unlock snapshot turn the app too.
- `send_slot_event()` maps touches into the app's coordinates.
- A warm resume that changes the shape goes through `ST_LAUNCH_WAIT`, so a
  frame of the old shape is never zoomed.

## Limits

- SpringBoard's own alerts and the CF card details dialog stay portrait over
  a landscape app.
- Screen mirroring (USB/VNC) and Presenter-to-Go show the LCD as it is, on
  its side.
- Apps in the sibling glaze-apps repository are not converted yet; they stay
  portrait until they call `ui_set_orientations`.

## Tests

- `make -C host gfx-test`: the quarter-turn blit against a per-pixel oracle.
- `make -C host ui-events-test`: the reshape at launch, ACTIVATE then RESIZE,
  and the scaled keyboard hitting the same keys.
- `tests/ui/t_landscape_calc.txt`: turns Calculator through the switcher and
  computes 7 + 5 with landscape taps.
- `tests/ui/t_landscape.txt`: Photos turned while in the background,
  Settings staying portrait, and a landscape tap opening a picture.
