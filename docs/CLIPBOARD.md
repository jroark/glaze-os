# Copy, cut and paste

Glaze has one text clipboard shared by its native apps. It lives in
SpringBoard's RAM, survives switching or closing apps, and is cleared by a
reboot. It is not written to a file or preference.

## On the device

1. Open an editable field and tap **Edit** in the keyboard's bottom row.
   With handwriting open, tap **Keys** first.
2. Tap **Select all**, then **Copy** or **Cut**. Selected text is highlighted
   in Notes and ordinary text fields.
3. Switch to another app, focus a field, open **Edit**, and tap **Paste**.
4. Tap **Keys** to return to typing. **Clear clip** discards the clipboard.

For a smaller selection, use **<** and **>** to position the cursor, tap
**Select**, then use the arrows to extend the selection. Tap Select again
to stop extending it. Notes also lets you tap the text to extend a selection
while Select is active. Typing or pasting replaces the selected range;
Backspace and Delete remove it. With a selection and Select off, an arrow
collapses it to that end before moving further.

The editing panel reports empty clipboards, failed transfers and text that
will not fit. Paste never inserts a truncated prefix. Cut removes the
selection only after copying it successfully.

## Supported fields

- Notes, including multiline text and saved edits.
- Browser's address/search bar, text inputs and textareas.
- Files' new-folder and rename fields.
- App Store search.
- Chat's message draft and model field.
- Gmail's account, search and compose fields, including the message body.
- AIM's account, buddy and message fields.
- Settings' Wi-Fi password field.

Password fields accept paste and selection replacement, but Copy and Cut are
disabled, including when a password is temporarily shown. Numeric, URL and
filename fields retain their input restrictions. Pasting newlines into a
single-line field converts them to spaces; multiline fields normalize CRLF
to LF. Tabs become spaces. An invalid filtered paste leaves the field intact.

This is text-field editing. It does not select arbitrary rendered web-page
text, conversation bubbles, images or files. Existing WinCE apps retain their
own compatibility-runtime clipboard; it is not connected to this clipboard.
There is no JavaScript Clipboard API or automatic clipboard sharing with the
computer. Older packages need rebuilding to opt into the touch Edit panel.

## Keyboard and mirroring

With a Stowaway keyboard, use **Ctrl+A**, **Ctrl+C**, **Ctrl+X** and **Ctrl+V**.
**Shift+Left/Right** extends the selection; **Shift+Home/End** selects to a
line boundary. Normal Home/End and Delete continue to work. The keyboard
layout and shortcuts have native decoder tests; physical dock validation
is still needed.

The same shortcuts work through `glaze-usb mirror`; **Command** can replace
Ctrl in a macOS VNC viewer. Modifier releases are tracked so releasing Shift
or Command before the other key still releases the correct Glaze key.
These operate on Glaze's clipboard. VNC ClientCutText remains ignored, so
copying in a desktop app does not supply a Glaze paste.

USB/network input scripts accept `key selectall`, `copy`, `cut`, `paste`,
`selectleft`, `selectright`, `selecthome` and `selectend`.

## SDK

Use the SDK and OS from the same revision. The existing `ui_keyboard`
structure and event structure sizes and prior key/IPC numbers are unchanged.
Legacy `ui_keyboard_init()` layouts do not emit new editing commands.

```c
static char text[256];
static int cursor;
static struct ui_keyboard keyboard;

ui_keyboard_edit_init(&keyboard, 0);
/* For a password use UI_EDIT_SECRET; for a textarea UI_EDIT_MULTILINE. */

/* Touch keyboard result (including editing commands): */
int key = ui_keyboard_event(&keyboard, &event, UI_H - UI_KB_H, &redraw);
if (key && ui_edit_key(text, sizeof(text), &cursor, key, 0)) {
    if (ui_edit_modified()) save_text();
    redraw = true;
}

/* Physical key/text events: */
if (ui_edit_event(text, sizeof(text), &cursor, &event, 0)) {
    if (ui_edit_modified()) save_text();
    redraw = true;
}
```

Use the same field flags in both paths: `UI_EDIT_SECRET`, `UI_EDIT_MULTILINE`,
`UI_EDIT_DIGITS`, `UI_EDIT_URL`, `UI_EDIT_FILENAME`, or `UI_EDIT_READONLY`.
Check `ui_edit_modified()` immediately after a handled event: selection,
Copy and cursor movement also return true without changing text. Use
`ui_draw_edit_selection()` before drawing text, with the full buffer and
the displayed range's byte offset. Clip it to the field. Secret fields
should keep their existing masked rendering.

There is one active selection per process. `ui_keyboard_edit_init()` resets
it; call `ui_edit_reset()` after programmatically replacing a buffer or
changing focus without initializing the keyboard. `ui_edit_set_cursor()`
places the cursor and either preserves or resets the selection anchor.
`ui_edit_selection()` returns the current byte range for custom rendering.
Helpers run on the UI thread, like the existing keyboard API.

`ui_clipboard_set(text, length)` copies up to `UI_CLIPBOARD_MAX` (8,192) bytes
and returns zero or a negative error. `ui_clipboard_get(out, capacity)` returns
the byte length, including zero for an empty clipboard, or a negative error.
Capacity includes the terminating NUL; insufficient space returns `GZ_ENOSPC`
without truncation. `ui_clipboard_clear()` discards text. Text uses Glaze's
existing 8-bit glyph/Latin-1 convention, not rich text or a new Unicode API.
Embedded NULs are rejected.

## Protocol and validation

The clipboard adds appended SpringBoard requests. Transfers use chunks in
the existing `sb_shared.req` area, keeping shared-memory layouts unchanged.
Writes have a transaction token and commit atomically after all bytes have
arrived; reads check a version on each chunk. Incomplete, expired, oversized
and out-of-order writes cannot replace the current clipboard. Pending writes
are scrubbed on commit, cancellation, a foreground change or a three-second
timeout. Access is limited to the foreground app through the usual
SpringBoard PID convention. As elsewhere in that protocol, the kernel does
not authenticate the claimed sender PID; this is not an isolation boundary
against malicious native apps. The API is not exposed to web scripts or
remote clipboard messages.

```sh
make -C host clipboard-test stowaway-test handwriting-test ui-events-test settings-test
make -C host mirror-test usb-input-test browser-test
make -j4 all glazeboot out/pkg/aim.gzp out/pkg/gmail.gzp out/pkg/chatgpt.gzp sdk-test
```

`clipboard-test` runs the actual client and server with deterministic IPC,
under ASan/UBSan: maximum-size cross-app transfer, small buffers, interrupted
and malformed writes, read-version changes, foreground changes, timeout
wraparound, secret and filtered fields, partial selection and replacement.
Keyboard tests cover opt-in/legacy layouts, cancellation and disabled commands;
mirroring/input tests cover shortcuts, releases and disconnect cleanup.

For an emulator UI check, build with `INPUT_SCRIPT=tests/t_clipboard.txt`,
boot a copy with `--speed 166`, and stop after 24 seconds. It copies
`GlazeClipboard` through Notes' touch Edit panel and pastes it into Browser's
address field. Rebuild with `INPUT_SCRIPT=` before using the image normally.
