# DoomGeneric

Source: https://github.com/ozkl/doomgeneric
Pinned commit: e257a09527062bfb986b6e5e0780839b6cbd5b71

Only the portable engine sources selected by upstream's Makefile and their
headers are vendored. No SDL/X11/Windows backends, executables or WADs are included.
Upstream is GPL-2.0-or-later (COPYING); this linked Glaze application elects GPLv3.
Copyright notices in the individual files remain in place.

Glaze-specific changes:

- Replace i_system.c, i_video.c and i_sound.c with the Glaze implementations in
  packages/doom/. No POSIX file/socket ABI changes: compat.h supplies a private,
  buffered stdio subset and uses the Glaze file service.
- doomgeneric.c does not allocate an unused RGBA frame buffer; video.c presents
  the engine's 320x200 indexed buffer through a RGB565 palette at 240x180.
- m_misc.c uses strtol instead of sscanf, Glaze's private mkdir, and bundle data
  for recovery saves. m_config.c uses strtol for integers, disables unused mouse
  float parsing, and puts saves/config paths under the selected IWAD's app-data
  directory. Upstream already compiles config file load/save out without ORIGCODE.
- Remove unused math.h includes and the relative mouse speed debug widget;
  timedemo statistics use integer arithmetic. No libm or libgcc is required.
- Replace Doom font and intermission numeric precision formats with equivalent
  zero-padded width formats supported by Glaze's formatter.
- m_menu.c accepts Enter / Escape for yes/no questions, making physical OK / Esc
  sufficient to navigate and quit without a keyboard.
- g_game.c checks save serialization and buffered close failures before replacing
  the previous save, and reports a failed rename instead of claiming success.
- r_defs.h, r_plane.c and r_segs.c use explicit in-array sentinel slots rather
  than indexing top/bottom arrays at -1 or SCREENWIDTH into adjacent padding.
  Found by the native UBSan gameplay test.

Compiler flags: MIPS2/O32/soft-float PIC, -fno-strict-aliasing and -fwrapv for
classic engine arithmetic. A few existing vendor warning categories are disabled
only for vendor objects. Native tests exclude shift and signed-overflow checks
for the engine's historical fixed-point arithmetic; ASan and other UBSan checks
remain enabled. Engine globals intentionally live until process exit.
