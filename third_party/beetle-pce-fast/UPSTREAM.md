# Beetle PCE Fast provenance and Glaze port

Source: https://github.com/libretro/beetle-pce-fast-libretro
Commit: `4eee99c451f85ebe66d1591de88b892015b23059`
License: GPL-2.0-or-later (source notices); GPLv2 text retained in `COPYING`.
The combined Glaze app elects GPLv3 and includes its license and source notice.

Selected upstream files: `mednafen/pce_fast/{huc6280.c,huc6280.h,huc6280_ops.h,
vdc.c,vdc.h,psg.c,psg.h,pce.h,huc.h}`. They provide the portable C interpreter,
VDC/VCE renderer and PSG logic. No libretro frontend, CD/CHD/codec stack, BIOS,
ROM, upstream test binary or desktop platform code is copied. Include wrappers
and adapter live in `packages/turbografx16`; shared UI/save code is in `lib/sega`.

Local changes:

- Replace the desktop Mednafen/libretro portability, surface, settings and state
  headers with the minimal Glaze definitions in this directory. Remove save-state
  functions/declarations and the CD synchronization path from the selected core.
  There are no CD firmware arrays or CD callback stubs in the build.
- Keep the C CPU interpreter, pointer-sized PC representation and lazy flags;
  remove unused state-only helpers/variables or mark them unused. Remove unused
  VDC chip variables and correct a settings signedness comparison for warnings.
- Replace floating-point grayscale palette setup with integer weighted luminance.
  Output uses fixed RGB565; custom desktop palette loading is not exposed.
- Replace PSG floating-point volume/table initialization with fixed gain and
  precomputed Q16 attenuation values. Retain the six-channel wave/noise/DDA/LFO
  logic. Replace Blip_Buffer with Glaze's original integer delta accumulator
  (`include/blip/Blip_Buffer.h` and `packages/turbografx16/audio.c`), which uses
  step reconstruction rather than a bandlimited filter. This changes sound quality.
- Avoid forming an out-of-range channel pointer for selectors 6/7, and replace
  left-shifting a negative LFO sample with multiplication by the same power of two
  (caught by UBSan register-edge tests).
- Normalize whitespace in copied source; omit legacy unimplemented CD entry
  declarations from the reduced header surface.

The cartridge bus follows the upstream libretro HuCard load/mapping path, with
bounded input validation and no filesystem dependency. It handles small ROM
mirrors, 384/512 KiB split layouts, 768 KiB absent upper space, backup RAM and
Populous RAM. Games use a single two-button controller. SFII's large mapper and
SuperGrafx/CD/Arcade Card are not implemented. See `docs/TURBOGRAFX16.md` for limits
and native/Glaze tests; original generated cartridges remain in ignored `out/`.
