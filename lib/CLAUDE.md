# Libraries

Read with the root CLAUDE.md, which covers building, testing and toolchain rules.

**UI libraries.** `libgfx` (`lib/gfx`) is a software RGB565 renderer for GZI images and GZF fonts; the formats are described in `lib/include/glaze/gfx.h`. `libui` (`lib/ui`) provides the nav bar, bar buttons, grouped cells, alerts, the keyboard, inertial scroll views and push/pop transitions. Apps follow the Settings/Notes pattern: draw the whole view, keep hit rects, `redraw()` after events, and use `ui_transition(draw_fn, forward)` for navigation. `host/` builds `libgfx` natively to render SpringBoard previews.

**Dynamic linking.** Apps and SpringBoard are PIE by default, sharing libc, gfx and UI through `lib/ldso` and `kernel/image.c`. `fs` and `net` stay static; `bin/audio` is dynamic. Read-only image frames are shared; writable data/GOT/BSS are private. Use the export maps and existing PIC rules for libraries; never embed another libc or static client archive in a plugin. `dlclose` releases an unreachable library after destructors: callers must stop its threads/callbacks and discard pointers first. No Linux compatibility, compiler TLS or C++ runtime. See `docs/os/DYNAMIC_LIBRARIES.md`. Run `make -C host elf-test`, `LD_TESTS=1 INIT=bin/ldtest`, and ordinary `ktest` after loader changes. Use `OUT=out/static DYNAMIC=0` for the fallback; keep build variants in separate output directories.

Handwriting input (`docs/os/HANDWRITING.md`) is shared by the existing keyboard
API: `lib/ui/handwriting.c` renders/captures ink and `recognize.c` performs
bounded integer point-cloud recognition using original `ink_shapes.h` data.
The new method byte uses `ui_keyboard`'s former padding; size and old field
offsets stay unchanged. Do not extend this structure without considering SDK
clients. Use `make -C host handwriting-test ui-events-test settings-test` and
`make sdk-test`; `tests/ui/t_handwriting_notes.txt` checks Notes with scripted input.
