# ClownMDEmu provenance and Glaze port

Source: https://github.com/Clownacy/clownmdemu
Commit: `88ef45a6585556e2247dd26b6c937d6b9fb4a12d`
License: AGPL-3.0-or-later, preserved in `LICENCE.txt`.

Dependencies copied from the upstream submodule revisions:

| Dependency | Source | Commit | License |
| --- | --- | --- | --- |
| Clown68000 | https://github.com/Clownacy/clown68000 | `bb198386ae3ee0cd6d246ff917f9941b02b5be6a` | AGPL-3.0-or-later, `libraries/clown68000/LICENCE.txt` |
| ClownZ80 | https://github.com/Clownacy/clownz80 | `aa4cae3127a5893ba7066bcc2d9e9ba8e88e1c3b` | AGPL-3.0-or-later, `libraries/clownz80/LICENCE.txt` |
| clowncommon | https://github.com/Clownacy/clowncommon | `ecee31fc78ee1eed901431d070239c9c6430607c` | ISC, `libraries/clowncommon/licence.txt` |

Only C sources/headers needed by the interpreter build are included. There are
no upstream test ROMs, disassemblers, desktop frontends or generated binaries.
`mega-cd-boot-rom.c` is deliberately excluded: its embedded boot ROM is not
part of this source tree. The corresponding bus include/array and read path
are removed; reads from that disabled path return 0xffff. Sega CD is disabled
by the frontend, as are all CD callbacks. Some CD source remains compiled
because upstream shares machine state and bus code across configurations.

The adapter and per-file build wrappers are in `packages/genesis`; frontend
and persistence are in `lib/sega`. Glaze uses the portable C interpreters,
integer FM/PSG tables, RGB565 output and a three-button controller. Runtime
floating point and a desktop library stack are unnecessary.

Local changes:

- Set common fixed-width types explicitly and fast integer types to 32 bits,
  so native LP64 tests and O32 MIPS builds use the same representation. Use
  the corresponding `%d`/`%u`/`%x` formats supported by Glaze's minimal libc.
  Resolve nested common-header paths to the single vendored copy and provide
  the compiler unreachable annotation used by the child cores.
- Remove unused math includes; table generators remain disabled source code.
  Initialize the VDP H-interrupt interval variable and strip trailing whitespace.
- Gate end-of-frame PCM/CDDA synchronization on the CD-enable flag, avoiding
  calls to absent CD callbacks in cartridge-only operation.
- Keep the controller strobe count at zero to expose a standard three-button
  controller; six-button inputs are not offered by this frontend.
- Supply a private O32 `setjmp`/`longjmp` implementation for 68000 exception
  dispatch, saving s0–s7, gp, sp, fp and ra. `platform/setjmp.h` redirects only
  this app; native tests use the host implementation. The original demo executes
  handled ILLEGAL and divide-by-zero exceptions followed by RTE to exercise it.

The frontend validates image/reset/save metadata, supplies big-endian ROM words,
mixes integer audio to 22050 Hz mono and supports ordinary SRAM. EEPROM, special
cartridge peripherals and CD/32X are outside this port. See `docs/SEGA.md` for
supported formats and validation limits. The combined app is distributed under
AGPLv3; package resources include the license, dependency notice and source URL.
No ROM binaries are vendored.
