# Testing on a real BE-300 (`tools/hwtest`)

`tools/hwtest` runs cases on a real BE-300 over the USB sync cable (or paired
network debugging) through `out/host/glaze-usb`. It builds GlazeBoot RAM images,
reloads them, drives input, reads the console, and compares screens with the
emulator. Physical steps (pulling the cable, the battery, swapping cards) are
listed as MANUAL cases and never faked.

```sh
make -C host glaze-usb
python3 -m tools.hwtest list
python3 -m tools.hwtest run                         # every suite but the NAND write
python3 -m tools.hwtest run --suite ui --suite power
python3 -m tools.hwtest run --case 'power/suspend'  # regex on suite/name
python3 -m tools.hwtest run --resume 20261107-183114            # skip what passed
python3 -m tools.hwtest run --suite ota --allow-flash           # the OTA (writes NAND)
python3 -m tools.hwtest run --host 192.168.68.84 --key ~/.config/glaze/DEBUG.KEY
```

Each run writes `out/hw/runs/<id>/`:
- `report.md` and `results.json`, with every case's status, detail and evidence
- `device.log`: the whole console. A pump drains it every 3 s, because `logs --once` empties the ring.
- `commands.log`: each glaze-usb call with its output
- `cases/<suite>-<name>/`: screenshots, diffs and console excerpts
- `state.json`: lets `--resume` continue after a hang

## Suites

| Suite | Image | What it checks |
|---|---|---|
| preflight | running | build, capabilities, battery (`power status`), the card in the slot |
| ktest | `ktest` | `INIT=bin/ktest` from a RAM boot: no FAIL, SKIPs recorded |
| boot | `default` | the LZ4 romfs pages in, SpringBoard ready time, unlock |
| ui | `default` | `tests/ui` home-screen editing scripts; the final screen against the emulator's |
| apps | `default` | each `.gzp` (this repo's `out/pkg`, glaze-apps' `out/pkg`) installed over USB, launched, poked, closed |
| power | `default` | Suspend with an alarm wake (30 s; 200 s past the CMCU watchdog), Power held, the idle power off, the front app resuming |
| usb | `default` | bus resets, a host killed mid-`put`/`logs`/`script`, a dead link; files read back by SHA-256 |
| card | `default` | per detected card: re-detection after Suspend, WL54-CF power save, GPS lock, CF storage |
| ota | NAND | stage a signed candidate through the maintenance package, the SPL installs it; then the LZ4 romfs from NAND, Hibernate through the SPL, the layout across a cold reset |
| installer | NAND | glaze-installer's `--worker maintenance-info/install-apps/uninstall-apps` |
| manual | — | steps for what needs hands |

Images are built once per source state into `out/hw/img-<name>` (`images.py`;
`default` is `GB_APPS=all` with `ramfs_kb=6656`). Edits under `tools/hwtest` and
`docs` don't trigger a rebuild. When a reload doesn't fit beside the running
apps, the runner boots the small `mini` image first.

## Device-side helpers

These keep power and link tests unattended (`lib/include/glaze/usb_wire.h`):

- **`glaze-usb power status`**: `struct gz_power_status` (battery band,
  adapter flags, network-card power time).
- **`glaze-usb power suspend|hibernate SECS`**: SpringBoard's own power off
  (Goodbye, the front app paused), with an RTC alarm SECS later
  (`SB_DEBUG_POWER`). A RAM boot suspends when asked to hibernate.
- **`glaze-usb power idle SECS [IDLE_MS]`**: arms the alarm and a short idle
  timeout. Once the host stops talking, SpringBoard's `debug_activity`
  hold-off ends and the real idle path powers the device off.
- **`glaze-usb usb-detach MS`**: turns the function unit off for MS ms after
  replying. On a real BE-300 the D+ pull-up stays, so the host keeps the device
  and sees its transfers fail. When the unit comes back without its address,
  only a bus reset brings it back. That's what the harness's `wait_device`
  (and glaze-usb's reload reconnect) do. A true unplug stays manual.
- A reload that doesn't fit first takes back unmapped romfs cache (kernel),
  then ends background apps (`SB_DEBUG_RECLAIM`), before refusing with a
  readable message.

Every sleep removes and reinserts the network card, which drops a Wi-Fi debug
session, so network runs reconnect after each power case.

## Screen comparison

`compare.py` decodes PNGs with numpy. The emulator's screenshot is darker:
it carries its backlight model, about 0.62× the device framebuffer. So the
reference is scaled by the median ratio of lit pixels first. Then it counts
pixels whose largest channel differs by more than 40, ignoring the status bar.
2% is the default limit. Emulator references are cached in `out/hw/emu-refs`.
Scripted input enters above the touch driver, so these cases test SpringBoard
and apps on the device, not the resistive panel (`tests/touch.sh` and
manual testing cover that).

## Adding a case

```python
from ..runner import case, Skip

@case('power', 'my-check', image='default', card=('wl54cf',), covers='#123')
def my_check(ctx):
    """One line for `list` and the report."""
    since = ctx.dev.log_offset()
    ctx.dev.script_text('500 tap 120 160', ctx.path('after.png'), name='tap')
    ctx.dev.wait_log(r'^something: happened', since=since, timeout=30)
    ctx.note('what the report should say')
```

A case fails on an `AssertionError` or a `DeviceError`. It also fails if the
console shows a panic or a fault during the case (`runner.BAD_LINES`).
`ctx.dev.pump_paused` stops the log pump, for cases that need the host silent
(sleep, idle, the installer).
