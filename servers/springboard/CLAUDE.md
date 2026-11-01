# SpringBoard

Read with the root CLAUDE.md, which covers building, testing and toolchain rules.

**Input.** Touch comes from the VRC4173 PIU, through GIRQ0 to GIU line 0 to IP2 (`kernel/drivers/irq.c`, `input.c`); buttons are polled and also arrive on GIRQ0. Events go into a kernel ring that only the bound compositor drains. SpringBoard forwards events to the foreground app through the shared ring in `struct sb_shared`. `ui_next_event` coalesces consecutive moves. Stowaway is detected once at boot
(`kernel/drivers/stowaway*.c`); UART RX is polled on the timer tick. `GZ_EV_TEXT`
and keyboard key events pass through SpringBoard to the foreground app.
`SYS_INPUT_STATUS` reports dock presence, passed in `SB_HELLO` reply word 4;
`ui_has_hardware_keyboard()` / `ui_keyboard_height()` separate editing focus
from on-screen keyboard layout. `make -C host stowaway-test` checks the decoder
and shared text editor.

**SpringBoard ↔ app protocol** (`glaze/sb.h`, `lib/ui/app.c`):
- An app sends `SB_HELLO` and receives a shared-memory 240x304 surface; the top 16 rows of the screen are SpringBoard's status bar.
- The app draws into the surface and calls `ui_present()`, which blocks until the dirty rectangle is on screen.
- Pressing Home moves the foreground app to the background (`GZ_EV_APP_DEACTIVATE`, `PROC_BACKGROUND` / `PRIO_LOW`); tapping its icon again warm-resumes it (`GZ_EV_APP_ACTIVATE`). libui first ends a touch in progress with `GZ_EV_TOUCH_CANCEL` (its last position; libui's scroll views, lists and keyboard drop it, and every app clears its own press state without acting), returns the DEACTIVATE, and then suspends the whole process (`GZ_PROC_SUSPENDED`) inside its next `ui_next_event` until ACTIVATE. SpringBoard freezes a background app itself `SUSPEND_MS` (1 s) after Home unless it called `ui_set_background_running(true)` (AIM and Gmail, which keep running at `PRIO_LOW`). DEACTIVATE is the moment to save and to release shared resources (Notes, Browser and Sketch flush pending saves; Video pauses, freeing the audio stream), because a suspended app may be ended without warning. Up to `SB_MAX_RUNNING` (4, all shown in the switcher) apps stay resident, with LRU and low-memory eviction (`LOW_MEM_PAGES` = 512). Slot 0 keeps its surface; the others unmap theirs (`gz_shm_unmap`) when their app ends. SpringBoard learns of exits from `GZ_NOTIFY_CHILD` rather than polling. Each install bumps the bundle's `.install-id` (`pkg_install_id`), so a reinstall, even at the same version, ends the app's background copy. Double-pressing Home on the home screen opens the bottom multitasking switcher bar (tap to switch, hold to enter quit mode with `-` badges).
- Passcode lock: with the `sb.passcode` preference set (salted hash, `glaze/passcode.h`, set from Settings → Passcode Lock), slide to unlock leads to `ST_PASSCODE`, a keypad drawn by libui's `ui_draw_passcode`; five wrong codes disable entry for a minute, doubling. `tests/ui/t_passcode.txt` exercises it.
- The home-screen app list is the `builtin_apps[]` table in `servers/springboard/draw.c` (icons are `assets/gen/icon_*.gzi`), copied into the runtime `sb_apps[]`. Installed apps (`servers/springboard/apps.c`) are appended from `pkg_list` of `/Applications` and `/Card/Applications`, rescanned on returning home and polled on the card every 2 s; an app keeps its index for the session (`!present` while its card is out, an empty `path` once uninstalled), since badges and notes are indexed by it. Holding an icon for 1 s enters jiggle mode, where installed apps get a delete button. SpringBoard reads an installed app's executable and spawns it with `gz_spawn_image`, and puts its bundle path in `sb_shared.bundle`; `ui_init` passes it to `res_set_bundle`, so `res_map` falls back to `<bundle>/res/`.
- Badges and notifications belong to SpringBoard (`servers/springboard/notify.c`), so they outlive the app. The front app calls `ui_set_badge` and `ui_notify_post/cancel/pending` (`lib/ui/notify.c`). Text travels in `sb_shared.req`, and fire times are `glaze/notify.h` wall-clock seconds. A note fires as a banner, on the lock screen when locked, or as `GZ_EV_APP_NOTIFY` with no badge when its own app is in front. Badges and pending notes persist as the `sb.badges`/`sb.notes` preferences, keyed by binary path. Other processes post for an app by path through `sb_*_for` (`lib/libc/notify.c`), which is unauthenticated because the kernel does not identify senders. Clock's timer and Calendar's reminders use it; `tests/ui/t_notify.txt` exercises it.

Hard icons (`docs/os/HARD_ICONS.md`) are the seven printed touch buttons below
the LCD (panel rows 320..359). Their zones and Windows CE's assignments come
from touch.dll and COShell's registry. SpringBoard captures strokes that start
there (`servers/springboard/hardicon.c`) and runs the `sb.icons` assignment
(`glaze/hardicon.h`, Settings → Icon Buttons). It never fires while locked.
`ui_claim_hard_icons` lets a front app receive `GZ_KEY_ICON1..7` instead,
through the appended `sb_shared.hard_icons`. Use `make -C host hardicon-test
settings-test` and `tests/ui/t_hardicon.txt`.

The text clipboard (`docs/os/CLIPBOARD.md`) is volatile SpringBoard state in
`servers/springboard/clipboard.c`, with chunked, atomic IPC through the existing
request area. `lib/ui/edit.c` owns one focused selection per process; apps opt
into keyboard commands with `ui_keyboard_edit_init` and route text through
`ui_edit_key`/`ui_edit_event`, using matching flags for secret/filtered fields.
Preserve the keyboard/shared-memory ABI. Run `clipboard-test stowaway-test
handwriting-test ui-events-test settings-test mirror-test usb-input-test`,
`browser-test`, full builds and `sdk-test`; `tests/ui/t_clipboard.txt` exercises
cross-app touch copy/paste. Clear INPUT_SCRIPT for normal builds afterward.

**Time** (`glaze/tz.h`, `glaze/ntp.h`, `lib/libc/tz.c`, `ntp.c`):
- The RTC holds local time, and everything (notifications, file times, the TLS date check) reads it as such. Zones are a table of cities with a standard offset and a US, EU or Australian daylight saving rule; `tz_local_to_utc`/`tz_utc_to_local` convert. Settings → Date & Time keeps the zone index in `set.zone` (default Los Angeles) and moves the RTC when it changes.
- With "Set Automatically" (`set.ntp`, default on), SpringBoard's `timesync.c` thread runs `ntp_sync` (SNTP over `glaze/net.h` UDP to `pool.ntp.org`, converted through the zone) once the network is up and every 6 hours, backing off after failures. Settings syncs at once when the switch is turned on, and notifies SpringBoard's port with `SB_NOTIFY_TIME` after changing the clock or these settings. `set.ntp_last` is the local time of the last sync.
