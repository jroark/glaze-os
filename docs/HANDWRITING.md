# Handwriting input

Glaze offers two on-screen text input methods: the existing QWERTY keyboard
and an offline handwriting pad. Tap **Write** on the keyboard's bottom row to
switch to handwriting, and **Keys** to switch back. Both occupy the same
240×150 area and return characters through the existing app input API.

To choose the default for newly initialized text panels, open
**Settings → Input Method → Handwriting** or **Keyboard**. Scroll down in
Settings if the row is below the screen. The page includes a practice area;
switching its small panel with Write/Keys does not change the saved default.
The selected default is stored as `ui.input_method`. Missing or invalid values
fall back to QWERTY. Existing open text panels keep their local selection.

## Using the pad

1. Print **one letter** in the white area. You can lift the pen between
   strokes, such as the crossbar of A or the dot of i.
2. Tap the correct character in the three choices above the ink. Nothing is
   inserted until you choose. Start the next character after that.
3. Use **^** to change case, **123/abc** to select digits or letters, and **?**
   to see example printed shapes. A blue ^ means uppercase; accepting a
   letter switches to lowercase, as with the keyboard's Shift key.

**Clear** discards uncommitted ink. Backspace, Space and Return operate on the
text being edited. In a URL field, the footer provides `.` and `/` plus Go;
the keyboard still provides its full URL layout and `.com` key. Switch to Keys
for other punctuation and symbols. Commands such as Space clear pending ink;
choose the letter first if you want to keep it.

The recognizer handles printed `a–z` and `0–9`, with several uppercase and
lowercase shape variants. Shape recognition and output case are separate:
you can draw an uppercase A shape and insert lowercase a with Shift off.
It recognizes individual characters, not cursive words or sentences. Similar
shapes can be ambiguous; the alternatives and separate letters/digits modes
help distinguish them. Accuracy depends on writing style; use the guide or
keyboard when the expected character is missing.

Handwriting works in Notes, Browser fields/URLs, Files dialogs, Wi-Fi password
entry, ChatGPT, mail/chat packages and other apps using `ui_keyboard_*`.
App-specific numeric keypads such as the lock-screen passcode pad are unchanged.
With a Stowaway keyboard attached, apps continue hiding the on-screen panel
according to the existing dock behaviour. Physical keyboard text entry is
unchanged.

## Recognition and lifecycle

Recognition runs entirely on the BE-300 with integer arithmetic. Original
hand-authored vector templates are normalized and compared against a resampled
point cloud. Normalization reduces sensitivity to position, size, pen speed,
stroke direction and stroke order; orientation and relative shape are retained.
No network service, model download, floating-point library or external API is
used. First use prepares about 8 KiB of per-process template data; raw ink and
candidates use a small bounded process-local buffer.

One character is limited to eight strokes and 256 points. Excess ink produces
no candidates until cleared. Blank/dot-only/incomplete ink is not committed.
Leaving the drawing area cancels the current stroke. Cancellation, application
deactivation, physical text input, or changing keyboard context discards pending
composition. Ink and candidates are never saved to disk; only the default
method is persisted. Pen strokes are silent; command/candidate keys use normal
keyboard clicks.

The event loop preserves sampled moves during an active ink stroke rather
than coalescing them as it does for scrolling. Normal scrolling keeps its
existing behaviour. The keyboard's existing fields retain their offsets and
the structure remains eight bytes; the new method byte occupies former padding.
Use `ui_keyboard_init` to initialize it.

## SDK

Apps using `ui_draw_keyboard` and `ui_keyboard_event` automatically gain the
new panel. They still receive one character on acceptance, backspace/return,
or `UI_KEY_DOTCOM` from the URL keyboard. The panel height remains `UI_KB_H`.
Use `ui_keyboard_set_method` to change a panel without saving a default, and
`ui_input_method_default` / `ui_input_method_save` for the shared preference.
These are UI-thread APIs, like the other libui widgets.

`<glaze/handwriting.h>` also provides the bounded `ui_ink` capture/recognition
API for a custom UI. Inputs use coordinates 0–1023 and recognition returns up
to three distinct candidates with a distance (lower is better). Rendering and
recognizer source ship in the standalone SDK's corresponding library source.

## Validation

```sh
make -C host handwriting-test ui-events-test settings-test gmail-test stowaway-test
make -j4 all glazeboot sdk-test
tests/run.sh tests/t_handwriting_notes.txt 18
```

Native ASan/UBSan tests use independent stylus traces, check all template glyphs,
translations/scales/reversed strokes, input limits, default preference errors,
context cancellation, keyboard switching, URL controls and structure bounds.
Event-loop tests check that ink samples survive queuing while normal moves are
still coalesced. Settings tests exercise the saved default, practice pad,
save errors, navigation, scrolling and rendering. The SDK test rebuilds libui
from its exported source.

The Notes script enters `Al` through real MIPS UI code and the kernel's scripted
input ring, saves it, returns to the list and reopens it. Boot a disposable NAND
copy: the emulator writes its flash image on shutdown. Rebuild normally after
using `INPUT_SCRIPT` so release images contain no scripted input.
These tests establish implementation behaviour, not a handwriting accuracy
rate across users. Physical stylus quality and latency still need testing on
a real BE-300.
