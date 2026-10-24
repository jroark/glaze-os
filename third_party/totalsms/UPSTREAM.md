# TotalSMS provenance and Glaze port

Source: https://github.com/ITotalJustice/TotalSMS
Commit: `05bbb8bbd931d21653cbb6dd7a75a5b198c86c29`
License: MIT, preserved in `LICENSE`.

The selected `src/core` C sources and headers include the Z80, bus, VDP, mapper
logic and cartridge CRC database. The single-file build and save-state code
are omitted. The ROM database is metadata, not cartridge data.

Scheduler source: https://github.com/ITotalJustice/scheduler
Version: v1.1.0, commit `ef78903bb91b681cd0febafb632c3f289a489ede`.
Its MIT license is preserved in `scheduler/LICENSE`.

Glaze's translation-unit wrappers and adapter are in `packages/mastersystem`;
its UI and persistence are in `lib/sega`. The upstream floating-point/blip PSG
dependency is not copied. `sn76489.h` is the compatibility interface for Glaze's
original integer implementation in `packages/mastersystem/psg.c` (GPLv3), with
timestamped writes, 16-clock tone counters, noise LFSR and 22050 Hz sampling.
It approximates PSG output and does not implement YM2413 FM.

Local changes:

- Use integer clock/rate calculations, Q8 volume, parity and H-counter math,
  avoiding floating-point compiler helpers and the unavailable parity helper.
- Bound header searches by the image length; recognize additional size codes;
  ensure an 8 KiB cartridge has at least one bank.
- Mirror ROM pages by image size, fixing small-ROM mapping independently of
  destination address; read and write 16-bit bus values through byte operations,
  including page/bank/0xffff boundaries and unaligned accesses on MIPS.
- Retain cartridge SRAM when rebuilding mapper state, enabling reset/TV changes
  without destroying saves. New games receive a newly allocated core.
- Permit the existing generic renderer to handle 224/240-line modes instead of
  asserting. The port bounds the framebuffer to 256x240.
- Guard unused helpers/scanline buffers for the fixed RGB565 configuration.

The Master System app exposes SMS/export mode. The separate Game Gear app
exposes GG/export mode, a cropped 160x144 LCD picture, 12-bit palette conversion,
polled Start and stereo-mask-aware PSG output downmixed to mono. Both apps share
`lib/sega/sms_core.c`; their package wrappers select hardware mode. Headerless
images require a known CRC matching the selected system.
Upstream database-selected mapper variants beyond Sega/Codemasters remain
unverified. See `docs/SEGA.md` and `tests/sms_banks.c` for tested boundaries.
The combined Glaze app is distributed under GPLv3; package resources contain
the applicable notices and licenses. No ROM binaries are vendored.
